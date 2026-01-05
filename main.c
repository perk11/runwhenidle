#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>

#include <wayland-client.h>
#include "ext-idle-notify-v1-client-protocol.h"

#include <X11/extensions/scrnsaver.h>

#include "sleep_utils.h"
#include "time_utils.h"
#include "tty_utils.h"
#include "process_handling.h"
#include "arguments_parsing.h"
#include "pause_methods.h"

#ifndef VERSION
#define VERSION 'unkown'
#endif

char *shell_command_to_run;
pid_t external_pid = 0;
int verbose = 0;
int quiet = 0;
int debug = 0;
int monitoring_started = 0;
enum pause_method pause_method = PAUSE_METHOD_SIGSTOP;
long start_monitor_after_ms = 300;
long unsigned user_idle_timeout_ms = 300000;
const long long POLLING_INTERVAL_MS = 1000;
const long long POLLING_INTERVAL_BEFORE_STARTING_MONITORING_MS = 100;
const char *pause_method_string[] = {
        //order must match order in pause_method enum
        [PAUSE_METHOD_SIGTSTP] = "SIGTSTP",
        [PAUSE_METHOD_SIGSTOP] = "SIGSTOP",
        NULL // Sentinel value to indicate the end of the array
};
int xscreensaver_is_available;
Display *x_display;
XScreenSaverInfo *xscreensaver_info;
const long unsigned IDLE_TIME_NOT_AVAILABLE_VALUE = ULONG_MAX;

volatile sig_atomic_t interruption_received = 0;
volatile sig_atomic_t command_paused = 0;
volatile sig_atomic_t sigchld_received = 0;
pid_t pid;

static int invoked_from_cron = 0;

static struct wl_display *wayland_display = NULL;
static struct wl_registry *wayland_registry = NULL;
static struct wl_seat *wayland_seat = NULL;
static struct ext_idle_notifier_v1 *wayland_idle_notifier = NULL;
static uint32_t wayland_idle_notifier_version = 0;
static struct ext_idle_notification_v1 *wayland_idle_notification = NULL;
static int wayland_idle_notify_available = 0;

static int is_string_null_or_empty(const char *value) {
    return value == NULL || value[0] == '\0';
}

static int file_is_socket(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISSOCK(st.st_mode) ? 1 : 0;
}

static int file_is_readable_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        return 0;
    }
    return access(path, R_OK) == 0 ? 1 : 0;
}

static int directory_exists_and_accessible(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return 0;
    }
    return access(path, R_OK | X_OK) == 0 ? 1 : 0;
}

static int read_process_status_ppid(pid_t target_pid, pid_t *out_ppid) {
    char status_path[64];
    snprintf(status_path, sizeof(status_path), "/proc/%d/status", target_pid);

    FILE *fp = fopen(status_path, "r");
    if (!fp) {
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "PPid:", 5) == 0) {
            long parsed = strtol(line + 5, NULL, 10);
            fclose(fp);
            if (parsed <= 0) {
                return 0;
            }
            *out_ppid = (pid_t)parsed;
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int read_process_comm(pid_t target_pid, char *out_comm, size_t out_comm_size) {
    char comm_path[64];
    snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", target_pid);

    FILE *fp = fopen(comm_path, "r");
    if (!fp) {
        return 0;
    }

    if (!fgets(out_comm, (int)out_comm_size, fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    out_comm[strcspn(out_comm, "\n")] = '\0';
    return 1;
}

static int comm_matches_any_cron_name(const char *comm) {
    const char *cron_names[] = { "cron", "crond", "anacron", "cronie", "fcron", NULL };
    for (int i = 0; cron_names[i] != NULL; i++) {
        if (strcmp(comm, cron_names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int detect_invoked_from_cron_via_process_tree(void) {
    pid_t current_pid = getpid();

    for (int depth = 0; depth < 12; depth++) {
        pid_t parent_pid = 0;
        if (!read_process_status_ppid(current_pid, &parent_pid)) {
            break;
        }
        if (parent_pid <= 1) {
            break;
        }

        char comm[128];
        if (read_process_comm(parent_pid, comm, sizeof(comm))) {
            if (comm_matches_any_cron_name(comm)) {
                return 1;
            }
        }

        current_pid = parent_pid;
    }

    return 0;
}

static int get_home_directory_for_current_user(char *out_home, size_t out_home_size) {
    const char *home_env = getenv("HOME");
    if (!is_string_null_or_empty(home_env)) {
        snprintf(out_home, out_home_size, "%s", home_env);
        return 1;
    }

    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_dir) {
        return 0;
    }

    snprintf(out_home, out_home_size, "%s", pw->pw_dir);
    return 1;
}

static int build_xauthority_path_from_home_dir(char *xauthority_path,
                                               size_t xauthority_path_size,
                                               const char *home_dir) {
    static const char xauthority_suffix[] = "/.Xauthority";
    size_t home_dir_length = strnlen(home_dir, xauthority_path_size);
    size_t suffix_length = sizeof(xauthority_suffix) - 1;

    if (home_dir_length == 0 || home_dir_length >= xauthority_path_size) {
        return 0;
    }

    if (home_dir_length + suffix_length + 1 > xauthority_path_size) {
        return 0;
    }

    memcpy(xauthority_path, home_dir, home_dir_length);
    memcpy(xauthority_path + home_dir_length, xauthority_suffix, suffix_length + 1);
    return 1;
}

static void ensure_xauthority_is_set_if_possible(void) {
    if (!is_string_null_or_empty(getenv("XAUTHORITY"))) {
        return;
    }

    char home_dir[PATH_MAX];
    if (!get_home_directory_for_current_user(home_dir, sizeof(home_dir))) {
        return;
    }

    char xauthority_path[PATH_MAX];
    if (!build_xauthority_path_from_home_dir(xauthority_path, sizeof(xauthority_path), home_dir)) {
        return;
    }

    if (file_is_readable_regular_file(xauthority_path)) {
        setenv("XAUTHORITY", xauthority_path, 0);
    }
}


static int build_default_xdg_runtime_dir_for_current_user(char *out_runtime_dir, size_t out_runtime_dir_size) {
    uid_t uid = getuid();
    snprintf(out_runtime_dir, out_runtime_dir_size, "/run/user/%u", (unsigned)uid);
    if (!directory_exists_and_accessible(out_runtime_dir)) {
        return 0;
    }
    return 1;
}

static int find_best_wayland_socket_in_runtime_dir(const char *runtime_dir,
                                                   char *out_socket_path,
                                                   size_t out_socket_path_size,
                                                   char *out_socket_name,
                                                   size_t out_socket_name_size) {
    DIR *dir = opendir(runtime_dir);
    if (!dir) {
        return 0;
    }

    int found = 0;
    int best_numeric_suffix = INT_MAX;
    char best_name[NAME_MAX + 1];
    best_name[0] = '\0';

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "wayland-", 8) != 0) {
            continue;
        }

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", runtime_dir, entry->d_name);

        if (!file_is_socket(full_path)) {
            continue;
        }

        int numeric_suffix = -1;
        const char *suffix = entry->d_name + 8;
        if (*suffix >= '0' && *suffix <= '9') {
            numeric_suffix = (int)strtol(suffix, NULL, 10);
        }

        if (!found) {
            found = 1;
            best_numeric_suffix = (numeric_suffix >= 0) ? numeric_suffix : INT_MAX;
            snprintf(best_name, sizeof(best_name), "%s", entry->d_name);
            continue;
        }

        if (numeric_suffix >= 0 && numeric_suffix < best_numeric_suffix) {
            best_numeric_suffix = numeric_suffix;
            snprintf(best_name, sizeof(best_name), "%s", entry->d_name);
            continue;
        }

        if (best_numeric_suffix == INT_MAX && numeric_suffix == -1) {
            snprintf(best_name, sizeof(best_name), "%s", entry->d_name);
        }
    }

    closedir(dir);

    if (!found) {
        return 0;
    }

    snprintf(out_socket_path, out_socket_path_size, "%s/%s", runtime_dir, best_name);
    if (out_socket_name && out_socket_name_size > 0) {
        snprintf(out_socket_name, out_socket_name_size, "%s", best_name);
    }
    return 1;
}

static int find_best_x11_display_from_socket_dir(char *out_display, size_t out_display_size) {
    const char *x11_socket_dir = "/tmp/.X11-unix";
    DIR *dir = opendir(x11_socket_dir);
    if (!dir) {
        return 0;
    }

    int found = 0;
    int best_display_number = INT_MAX;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != 'X') {
            continue;
        }
        const char *digits = entry->d_name + 1;
        if (*digits < '0' || *digits > '9') {
            continue;
        }

        int display_number = (int)strtol(digits, NULL, 10);

        char socket_path[PATH_MAX];
        snprintf(socket_path, sizeof(socket_path), "%s/%s", x11_socket_dir, entry->d_name);

        if (!file_is_socket(socket_path)) {
            continue;
        }

        if (!found || display_number < best_display_number) {
            found = 1;
            best_display_number = display_number;
        }
    }

    closedir(dir);

    if (!found) {
        return 0;
    }

    snprintf(out_display, out_display_size, ":%d", best_display_number);
    return 1;
}

static void best_effort_infer_graphical_session_environment_if_missing(void) {
    if (!is_string_null_or_empty(getenv("WAYLAND_DISPLAY")) || !is_string_null_or_empty(getenv("DISPLAY"))) {
        return;
    }

    char inferred_runtime_dir[PATH_MAX];
    if (is_string_null_or_empty(getenv("XDG_RUNTIME_DIR"))) {
        if (build_default_xdg_runtime_dir_for_current_user(inferred_runtime_dir, sizeof(inferred_runtime_dir))) {
            setenv("XDG_RUNTIME_DIR", inferred_runtime_dir, 0);
        }
    }

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!is_string_null_or_empty(runtime_dir) && is_string_null_or_empty(getenv("WAYLAND_DISPLAY"))) {
        char wayland_socket_path[PATH_MAX];
        char wayland_socket_name[NAME_MAX + 1];
        if (find_best_wayland_socket_in_runtime_dir(runtime_dir, wayland_socket_path, sizeof(wayland_socket_path),
                                                    wayland_socket_name, sizeof(wayland_socket_name))) {
            setenv("WAYLAND_DISPLAY", wayland_socket_name, 0);
        }
    }

    if (is_string_null_or_empty(getenv("DISPLAY"))) {
        char inferred_display[32];
        if (find_best_x11_display_from_socket_dir(inferred_display, sizeof(inferred_display))) {
            setenv("DISPLAY", inferred_display, 0);
            ensure_xauthority_is_set_if_possible();
        }
    }

    if (verbose && invoked_from_cron) {
        const char *wd = getenv("WAYLAND_DISPLAY");
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        const char *dpy = getenv("DISPLAY");
        fprintf(stderr, "Detected cron context; inferred session vars: XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s DISPLAY=%s\n",
                xdg ? xdg : "(unset)", wd ? wd : "(unset)", dpy ? dpy : "(unset)");
    }
}

static int open_pid_file_descriptor_for_process(pid_t process_id) {
#if defined(SYS_pidfd_open)
    return (int)syscall(SYS_pidfd_open, process_id, 0);
#elif defined(__NR_pidfd_open)
    return (int)syscall(__NR_pidfd_open, process_id, 0);
#else
    (void)process_id;
    errno = ENOSYS;
    return -1;
#endif
}

static int create_one_shot_timer_file_descriptor_after_ms(long delay_ms) {
    int timer_file_descriptor = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_file_descriptor < 0) {
        return -1;
    }

    if (delay_ms < 0) {
        delay_ms = 0;
    }

    struct itimerspec timer_spec;
    memset(&timer_spec, 0, sizeof(timer_spec));
    timer_spec.it_value.tv_sec = delay_ms / 1000;
    timer_spec.it_value.tv_nsec = (delay_ms % 1000) * 1000000L;

    if (timerfd_settime(timer_file_descriptor, 0, &timer_spec, NULL) < 0) {
        close(timer_file_descriptor);
        return -1;
    }

    return timer_file_descriptor;
}

static int create_periodic_timer_file_descriptor_every_ms(long interval_ms) {
    int timer_file_descriptor = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_file_descriptor < 0) {
        return -1;
    }

    if (interval_ms <= 0) {
        interval_ms = 1000;
    }

    struct itimerspec timer_spec;
    memset(&timer_spec, 0, sizeof(timer_spec));
    timer_spec.it_value.tv_sec = interval_ms / 1000;
    timer_spec.it_value.tv_nsec = (interval_ms % 1000) * 1000000L;
    timer_spec.it_interval = timer_spec.it_value;

    if (timerfd_settime(timer_file_descriptor, 0, &timer_spec, NULL) < 0) {
        close(timer_file_descriptor);
        return -1;
    }

    return timer_file_descriptor;
}

static void consume_timer_file_descriptor(int timer_file_descriptor) {
    uint64_t expirations = 0;
    (void)read(timer_file_descriptor, &expirations, sizeof(expirations));
}

long unsigned query_user_idle_time() {
    if (xscreensaver_is_available) {
        XScreenSaverQueryInfo(x_display, DefaultRootWindow(x_display), xscreensaver_info);
        return xscreensaver_info->idle;
    }

    return IDLE_TIME_NOT_AVAILABLE_VALUE;
}

int handle_interruption() {
    if (command_paused) {
        if (verbose) {
            fprintf(stderr, "Since command was previously paused, we will try to resume it now");
            if (!external_pid) {
                fprintf(stderr, " to be able to handle the interruption before exiting");
            }
            fprintf(stderr, "\n");
        }
        resume_command_recursively(pid);
    }
    if (external_pid) {
        return 0;
    }
    //Wait for the child process to complete
    return wait_for_pid_to_exit_synchronously(pid);
}

void sigint_handler(int signum) {
    if (external_pid) {
        if (!quiet) {
            printf("Received SIGINT\n");
        }
    } else {
        if (!quiet) {
            printf("Received SIGINT, sending SIGINT to the command and waiting for it to finish\n");
        }
        send_signal_to_pid(pid, signum, "SIGINT");
    }

    interruption_received = 1;
}

void sigterm_handler(int signum) {
    if (external_pid) {
        if (!quiet) {
            printf("Received SIGTERM\n");
        }
    } else {
        if (!quiet) {
            printf("Received SIGTERM, sending SIGTERM to the command and waiting for it to finish\n");
        }
        send_signal_to_pid(pid, signum, "SIGTERM");
    }

    interruption_received = 1;
}

void sigchld_handler(int signum) {
    (void)signum;
    sigchld_received = 1;
}

static void wayland_idle_notification_idled(void *data, struct ext_idle_notification_v1 *notification) {
    (void)data;
    (void)notification;

    if (!monitoring_started) {
        return;
    }

    if (debug) {
        fprintf(stderr, "Wayland idle: idled()\n");
    }

    if (command_paused) {
        if (verbose) {
            fprintf(stderr, "Wayland idle: resuming command\n");
        }
        if (!quiet) {
            printf("Lack of user activity detected. ");
        }
        resume_command_recursively(pid);
        command_paused = 0;
    }
}

static void wayland_idle_notification_resumed(void *data, struct ext_idle_notification_v1 *notification) {
    (void)data;
    (void)notification;

    if (!monitoring_started) {
        return;
    }

    if (debug) {
        fprintf(stderr, "Wayland idle: resumed()\n");
    }

    if (!command_paused) {
        if (verbose) {
            fprintf(stderr, "Wayland idle: pausing command\n");
        }
        pause_command_recursively(pid);
        command_paused = 1;
    }
}

static const struct ext_idle_notification_v1_listener wayland_idle_notification_listener = {
        .idled = wayland_idle_notification_idled,
        .resumed = wayland_idle_notification_resumed
};

static void wayland_registry_global(void *data,
                                    struct wl_registry *registry,
                                    uint32_t name,
                                    const char *interface,
                                    uint32_t version) {
    (void)data;

    if (strcmp(interface, "wl_seat") == 0 && wayland_seat == NULL) {
        wayland_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        return;
    }

    if (strcmp(interface, "ext_idle_notifier_v1") == 0 && wayland_idle_notifier == NULL) {
        uint32_t bind_version = version < 2 ? version : 2;
        wayland_idle_notifier_version = bind_version;
        wayland_idle_notifier = wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, bind_version);
        return;
    }
}

static void wayland_registry_global_remove(void *data,
                                           struct wl_registry *registry,
                                           uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener wayland_registry_listener = {
        .global = wayland_registry_global,
        .global_remove = wayland_registry_global_remove
};

static struct wl_display *connect_to_wayland_best_effort(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (display) {
        return display;
    }

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    char inferred_runtime_dir[PATH_MAX];
    if (is_string_null_or_empty(runtime_dir)) {
        if (!build_default_xdg_runtime_dir_for_current_user(inferred_runtime_dir, sizeof(inferred_runtime_dir))) {
            return NULL;
        }
        runtime_dir = inferred_runtime_dir;
    }

    char wayland_socket_path[PATH_MAX];
    char wayland_socket_name[NAME_MAX + 1];
    if (!find_best_wayland_socket_in_runtime_dir(runtime_dir, wayland_socket_path, sizeof(wayland_socket_path),
                                                 wayland_socket_name, sizeof(wayland_socket_name))) {
        return NULL;
    }

    display = wl_display_connect(wayland_socket_path);
    if (!display) {
        return NULL;
    }

    if (is_string_null_or_empty(getenv("XDG_RUNTIME_DIR"))) {
        setenv("XDG_RUNTIME_DIR", runtime_dir, 0);
    }
    if (is_string_null_or_empty(getenv("WAYLAND_DISPLAY"))) {
        setenv("WAYLAND_DISPLAY", wayland_socket_name, 0);
    }

    return display;
}

static int try_initialize_wayland_idle_backend(void) {
    wayland_display = connect_to_wayland_best_effort();
    if (!wayland_display) {
        return 0;
    }

    wayland_registry = wl_display_get_registry(wayland_display);
    if (!wayland_registry) {
        wl_display_disconnect(wayland_display);
        wayland_display = NULL;
        return 0;
    }

    wl_registry_add_listener(wayland_registry, &wayland_registry_listener, NULL);
    wl_display_roundtrip(wayland_display);

    if (wayland_seat == NULL || wayland_idle_notifier == NULL) {
        wl_registry_destroy(wayland_registry);
        wayland_registry = NULL;
        wl_display_disconnect(wayland_display);
        wayland_display = NULL;
        wayland_seat = NULL;
        wayland_idle_notifier = NULL;
        wayland_idle_notify_available = 0;
        return 0;
    }

    wayland_idle_notify_available = 1;
    return 1;
}

static int start_wayland_idle_notification_object(void) {
    if (!wayland_idle_notify_available || wayland_idle_notification != NULL) {
        return 0;
    }

    uint32_t timeout_ms_for_protocol = (user_idle_timeout_ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)user_idle_timeout_ms;

    if (wayland_idle_notifier_version >= 2) {
        wayland_idle_notification = ext_idle_notifier_v1_get_input_idle_notification(
                wayland_idle_notifier, timeout_ms_for_protocol, wayland_seat);
    } else {
        wayland_idle_notification = ext_idle_notifier_v1_get_idle_notification(
                wayland_idle_notifier, timeout_ms_for_protocol, wayland_seat);
    }

    if (!wayland_idle_notification) {
        return -1;
    }

    ext_idle_notification_v1_add_listener(wayland_idle_notification, &wayland_idle_notification_listener, NULL);
    wl_display_flush(wayland_display);
    return 1;
}

static int run_wayland_idle_event_loop(void) {
    int start_monitor_timer_file_descriptor = create_one_shot_timer_file_descriptor_after_ms(start_monitor_after_ms);
    int wayland_file_descriptor = wl_display_get_fd(wayland_display);

    int process_exit_wait_file_descriptor = open_pid_file_descriptor_for_process(pid);
    int external_pid_fallback_check_timer_file_descriptor = -1;
    if (process_exit_wait_file_descriptor < 0 && external_pid != 0) {
        external_pid_fallback_check_timer_file_descriptor = create_periodic_timer_file_descriptor_every_ms(1000);
    }

    struct pollfd poll_file_descriptors[4];
    int poll_file_descriptor_count = 0;

    int wayland_poll_index = poll_file_descriptor_count++;
    poll_file_descriptors[wayland_poll_index] = (struct pollfd){ .fd = wayland_file_descriptor, .events = POLLIN, .revents = 0 };

    int start_monitor_poll_index = poll_file_descriptor_count++;
    poll_file_descriptors[start_monitor_poll_index] = (struct pollfd){ .fd = start_monitor_timer_file_descriptor, .events = POLLIN, .revents = 0 };

    int process_exit_poll_index = -1;
    if (process_exit_wait_file_descriptor >= 0) {
        process_exit_poll_index = poll_file_descriptor_count++;
        poll_file_descriptors[process_exit_poll_index] = (struct pollfd){ .fd = process_exit_wait_file_descriptor, .events = POLLIN, .revents = 0 };
    }

    int external_pid_fallback_poll_index = -1;
    if (external_pid_fallback_check_timer_file_descriptor >= 0) {
        external_pid_fallback_poll_index = poll_file_descriptor_count++;
        poll_file_descriptors[external_pid_fallback_poll_index] = (struct pollfd){
                .fd = external_pid_fallback_check_timer_file_descriptor, .events = POLLIN, .revents = 0
        };
    }

    while (1) {
        if (interruption_received) {
            if (start_monitor_timer_file_descriptor >= 0) close(start_monitor_timer_file_descriptor);
            if (process_exit_wait_file_descriptor >= 0) close(process_exit_wait_file_descriptor);
            if (external_pid_fallback_check_timer_file_descriptor >= 0) close(external_pid_fallback_check_timer_file_descriptor);
            return handle_interruption();
        }

        if (sigchld_received) {
            sigchld_received = 0;
            exit_if_pid_has_finished(pid);
        }

        wl_display_dispatch_pending(wayland_display);
        wl_display_flush(wayland_display);

        int poll_result = poll(poll_file_descriptors, poll_file_descriptor_count, -1);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf_error("poll() failed: %s\n", strerror(errno));
            return 1;
        }

        if (!monitoring_started && (poll_file_descriptors[start_monitor_poll_index].revents & POLLIN)) {
            consume_timer_file_descriptor(start_monitor_timer_file_descriptor);
            monitoring_started = 1;

            if (start_wayland_idle_notification_object() < 0) {
                fprintf_error("Failed to create Wayland idle notification object, user will be considered idle.\n");
            } else {
                if (!command_paused) {
                    pause_command_recursively(pid);
                    command_paused = 1;
                }
            }
        }

        if (process_exit_poll_index >= 0 && (poll_file_descriptors[process_exit_poll_index].revents & POLLIN)) {
            exit_if_pid_has_finished(pid);
        }

        if (external_pid_fallback_poll_index >= 0 && (poll_file_descriptors[external_pid_fallback_poll_index].revents & POLLIN)) {
            consume_timer_file_descriptor(external_pid_fallback_check_timer_file_descriptor);
            exit_if_pid_has_finished(pid);
        }

        if (poll_file_descriptors[wayland_poll_index].revents & POLLIN) {
            int dispatch_result = wl_display_dispatch(wayland_display);
            if (dispatch_result < 0 && errno != EINTR) {
                fprintf_error("Wayland display dispatch failed: %s\n", strerror(errno));
                fprintf_error("User will be considered idle to allow the command to finish.\n");
                break;
            }
        } else if (poll_file_descriptors[wayland_poll_index].revents & (POLLHUP | POLLERR)) {
            fprintf_error("Wayland connection closed, user will be considered idle to allow the command to finish.\n");
            break;
        }
    }

    if (start_monitor_timer_file_descriptor >= 0) close(start_monitor_timer_file_descriptor);
    if (process_exit_wait_file_descriptor >= 0) close(process_exit_wait_file_descriptor);
    if (external_pid_fallback_check_timer_file_descriptor >= 0) close(external_pid_fallback_check_timer_file_descriptor);

    while (1) {
        if (interruption_received) {
            return handle_interruption();
        }
        if (sigchld_received) {
            sigchld_received = 0;
            exit_if_pid_has_finished(pid);
        }
        exit_if_pid_has_finished(pid);
        sleep_for_milliseconds(250);
    }
}

static Display *open_x11_display_best_effort(void) {
    Display *display = XOpenDisplay(NULL);
    if (display) {
        return display;
    }

    if (!is_string_null_or_empty(getenv("DISPLAY"))) {
        return NULL;
    }

    char inferred_display[32];
    if (!find_best_x11_display_from_socket_dir(inferred_display, sizeof(inferred_display))) {
        return NULL;
    }

    setenv("DISPLAY", inferred_display, 0);
    ensure_xauthority_is_set_if_possible();
    return XOpenDisplay(NULL);
}

int main(int argc, char *argv[]) {
    parse_command_line_arguments(argc, argv);

    invoked_from_cron = detect_invoked_from_cron_via_process_tree();

    if (external_pid == 0) {
        pid = run_shell_command(shell_command_to_run);
    } else {
        pid = external_pid;
        if (kill(pid, 0) == -1) {
            fprintf_error("PID %d is not running\n", pid);
            exit(1);
        }
    }
    free(shell_command_to_run);

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigterm_handler);
    signal(SIGCHLD, sigchld_handler);

    best_effort_infer_graphical_session_environment_if_missing();

    if (try_initialize_wayland_idle_backend()) {
        int wayland_loop_result = run_wayland_idle_event_loop();

        if (wayland_idle_notification) {
            ext_idle_notification_v1_destroy(wayland_idle_notification);
            wayland_idle_notification = NULL;
        }
        if (wayland_idle_notifier) {
            ext_idle_notifier_v1_destroy(wayland_idle_notifier);
            wayland_idle_notifier = NULL;
        }
        if (wayland_seat) {
            wl_seat_destroy(wayland_seat);
            wayland_seat = NULL;
        }
        if (wayland_registry) {
            wl_registry_destroy(wayland_registry);
            wayland_registry = NULL;
        }
        if (wayland_display) {
            wl_display_disconnect(wayland_display);
            wayland_display = NULL;
        }

        return wayland_loop_result;
    }

    x_display = open_x11_display_best_effort();
    if (!x_display) {
        xscreensaver_is_available = 0;
        fprintf_error("Couldn't open an X11 display!\n");
    } else {
        int xscreensaver_event_base, xscreensaver_error_base;
        xscreensaver_is_available = XScreenSaverQueryExtension(
                x_display, &xscreensaver_event_base, &xscreensaver_error_base);
        if (xscreensaver_is_available) {
            xscreensaver_info = XScreenSaverAllocInfo();
        }
    }

    if (!xscreensaver_is_available) {
        fprintf_error("No available method for detecting user idle time on the system, user will be considered idle to allow the command to finish.\n");
    }

    struct timespec time_when_command_started;
    clock_gettime(CLOCK_MONOTONIC, &time_when_command_started);

    long long sleep_time_ms = POLLING_INTERVAL_BEFORE_STARTING_MONITORING_MS;
    unsigned long user_idle_time_ms = 0;

    if (verbose) {
        fprintf(stderr, "Starting to monitor user activity (X11 polling)\n");
    }

    while (1) {
        if (interruption_received) {
            return handle_interruption();
        }
        if (sigchld_received) {
            sigchld_received = 0;
            exit_if_pid_has_finished(pid);
        }
        if (!monitoring_started) {
            struct timespec current_time;
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            long long elapsed_ms = get_elapsed_time_ms(time_when_command_started, current_time);
            if (debug) fprintf(stderr, "%lldms elapsed since command started\n", elapsed_ms);
            if (elapsed_ms >= start_monitor_after_ms) {
                monitoring_started = 1;
            }
        }
        if (monitoring_started) {
            user_idle_time_ms = query_user_idle_time();
        }
        // Checking this after querying the screensaver timer so that the command is still running while
        // we're querying the screensaver and has a chance to do some work and finish,
        // but before potentially pausing the command to avoid trying to pause it if it completed.
        exit_if_pid_has_finished(pid);

        if (monitoring_started) {
            if (user_idle_time_ms >= user_idle_timeout_ms) {
                if (command_paused) {
                    if (!quiet) {
                        printf("Lack of user activity detected. ");
                    }
                    resume_command_recursively(pid);
                    command_paused = 0;
                }
            } else {
                if (!command_paused) {
                    pause_command_recursively(pid);
                    command_paused = 1;
                }
            }
        }
        if (debug) fprintf(stderr, "Sleeping for %lldms\n", sleep_time_ms);
        sleep_for_milliseconds(sleep_time_ms);
    }
}
