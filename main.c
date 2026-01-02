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

static struct wl_display *wayland_display = NULL;
static struct wl_registry *wayland_registry = NULL;
static struct wl_seat *wayland_seat = NULL;
static struct ext_idle_notifier_v1 *wayland_idle_notifier = NULL;
static uint32_t wayland_idle_notifier_version = 0;
static struct ext_idle_notification_v1 *wayland_idle_notification = NULL;
static int wayland_idle_notify_available = 0;

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

long long pause_or_resume_command_depending_on_user_activity(
        long long sleep_time_ms,
        unsigned long user_idle_time_ms) {
    if (user_idle_time_ms >= user_idle_timeout_ms) {
        if (debug)
            fprintf(stderr, "Idle time: %lums, idle timeout: %lums, user is inactive\n", user_idle_time_ms,
                    user_idle_timeout_ms);
        if (command_paused) {
            sleep_time_ms = POLLING_INTERVAL_MS; //reset to default value
            if (verbose) {
                fprintf(stderr, "Idle time: %lums, idle timeout: %lums, resuming command\n", user_idle_time_ms,
                        user_idle_timeout_ms);
            }
            if (!quiet) {
                printf("Lack of user activity detected. ");
                //intentionally no new line here, resume_command will print the rest of the message.
            }
            resume_command_recursively(pid);
            command_paused = 0;
        }
    } else {
        struct timespec time_when_starting_to_pause;
        int command_was_paused_this_iteration = 0;
        // User is active
        if (!command_paused) {
            clock_gettime(CLOCK_MONOTONIC, &time_when_starting_to_pause);
            if (verbose) {
                fprintf(stderr, "Idle time: %lums.\n", user_idle_time_ms);
            }
            pause_command_recursively(pid);
            if (debug) fprintf(stderr, "Command paused\n");
            command_paused = 1;
            command_was_paused_this_iteration = 1;
        }
        sleep_time_ms = user_idle_timeout_ms - user_idle_time_ms;
        if (debug) fprintf(stderr, "Target sleep time: %llums\n", sleep_time_ms);
        if (command_was_paused_this_iteration) {
            if (debug) fprintf(stderr, "Command was paused this iteration\n");
            struct timespec time_before_sleep;
            clock_gettime(CLOCK_MONOTONIC, &time_before_sleep);
            long long pausing_time_ms = get_elapsed_time_ms(time_when_starting_to_pause, time_before_sleep);
            if (debug)
                fprintf(stderr,
                        "Target sleep time before taking into account time it took to pause: %lldms, time it took to pause: %lldms\n",
                        sleep_time_ms, pausing_time_ms);
            sleep_time_ms = sleep_time_ms - pausing_time_ms;

        }

        if (sleep_time_ms < POLLING_INTERVAL_MS) {
            if (debug)
                fprintf(stderr,
                        "Target sleep time %lldms is less than polling interval %lldms, resetting it to polling interval\n",
                        sleep_time_ms, POLLING_INTERVAL_MS);
            sleep_time_ms = POLLING_INTERVAL_MS;
        }
        if (verbose) {
            fprintf(
                    stderr,
                    "Polling every second is temporarily disabled due to user activity, idle time: %lums, next activity check scheduled in %lldms\n",
                    user_idle_time_ms,
                    sleep_time_ms
            );
        }
    }
    return sleep_time_ms;
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

static int try_initialize_wayland_idle_backend(void) {
    wayland_display = wl_display_connect(NULL);
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
        if (wayland_registry) {
            wl_registry_destroy(wayland_registry);
            wayland_registry = NULL;
        }
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

    if (verbose) {
        fprintf(stderr, "Wayland backend: waiting for idle notifications\n");
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

            if (verbose) {
                fprintf(stderr, "Starting to monitor user activity (Wayland ext-idle-notify-v1)\n");
            }

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

int main(int argc, char *argv[]) {
    parse_command_line_arguments(argc, argv);

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

    x_display = XOpenDisplay(NULL);
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
            sleep_time_ms = pause_or_resume_command_depending_on_user_activity(
                    sleep_time_ms,
                    user_idle_time_ms);
        }
        if (debug) fprintf(stderr, "Sleeping for %lldms\n", sleep_time_ms);
        sleep_for_milliseconds(sleep_time_ms);
    }
}
