#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>

#include <wayland-client.h>

#include <X11/extensions/scrnsaver.h>

#include "environment_guessing.h"
#include "sleep_utils.h"
#include "time_utils.h"
#include "tty_utils.h"
#include "process_handling.h"
#include "arguments_parsing.h"
#include "descriptor_utils.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "pause_methods.h"
#include "wayland.h"

#ifndef VERSION
#define VERSION "unknown"
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
const long long POLLING_INTERVAL_WHEN_NOT_MONITORING_MS = 100;
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
        command_paused = 0;
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
static void resume_paused_command_on_user_idle(void) {
    if (!quiet) {
        printf("Lack of user activity detected. ");
        //intentionally no new line here, resume_command will print the rest of the message.
    }
    resume_command_recursively(pid);
    command_paused = 0;
}

static void pause_running_command_on_user_activity(void) {
    pause_command_recursively(pid);
    if (debug) fprintf(stderr, "Command paused\n");
    command_paused = 1;
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

    resume_paused_command_on_user_idle();
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
        pause_running_command_on_user_activity();
    }
}

int resume_and_wait_for_pid_to_exit_checking_for_signals(void) {
    if (command_paused) {
        if (verbose) {
            fprintf(stderr, "Since command was previously paused, we will try to resume it now to let it finish\n");
        }
        command_paused = 0;
        resume_command_recursively(pid);
    }
    while (1) {
        if (interruption_received) {
            return handle_interruption();
        }
        if (sigchld_received) {
            sigchld_received = 0;
        }
        exit_if_pid_has_finished(pid);
        sleep_for_milliseconds(POLLING_INTERVAL_WHEN_NOT_MONITORING_MS);
    }
}
const struct ext_idle_notification_v1_listener wayland_idle_notification_listener = {
    .idled = wayland_idle_notification_idled,
    .resumed = wayland_idle_notification_resumed
};

int run_wayland_idle_event_loop(struct wl_display *wayland_display) {
    int result = -1;
    int wayland_flush_is_pending = 0;

    int start_monitor_timer_file_descriptor = create_one_shot_timer_file_descriptor_after_ms(start_monitor_after_ms);
    int process_exit_wait_file_descriptor = -1;
    int external_pid_fallback_check_timer_file_descriptor = -1;

    if (start_monitor_timer_file_descriptor == -1) {
        const int saved_errno = errno;
        fprintf_error("Wayland idle event loop aborted: failed to create a timer file descriptor: %s\n",
                      strerror(saved_errno));
        goto run_wayland_idle_event_loop_cleanup;
    }

    const int wayland_display_file_descriptor = wl_display_get_fd(wayland_display);
    if (wayland_display_file_descriptor == -1) {
        fprintf_error("Wayland idle event loop aborted: failed to get Wayland display file descriptor\n");
        goto run_wayland_idle_event_loop_cleanup;
    }

    process_exit_wait_file_descriptor = open_pid_file_descriptor_for_process(pid);
    if (process_exit_wait_file_descriptor == -1) {
        const int saved_errno = errno;
        fprintf_error(
            "Failed to open file descriptor for pid %d: %s, falling back to a timer every %sms for checking if process has exited\n",
            pid,
            strerror(saved_errno),
            POLLING_INTERVAL_MS
        );
        external_pid_fallback_check_timer_file_descriptor = create_periodic_timer_file_descriptor_every_ms(POLLING_INTERVAL_MS);
        if (external_pid_fallback_check_timer_file_descriptor == -1) {
            const int timer_errno = errno;
            fprintf_error("Failed to create periodic timer file descriptor for external pid fallback: %s\n",
                          strerror(timer_errno));
            goto run_wayland_idle_event_loop_cleanup;
        }
    }

    struct pollfd poll_file_descriptors[4];
    int poll_file_descriptor_count = 0;

    const int wayland_poll_index = poll_file_descriptor_count++;
    poll_file_descriptors[wayland_poll_index] = (struct pollfd){
            .fd = wayland_display_file_descriptor,
            .events = POLLIN,
            .revents = 0
    };

    const int start_monitor_poll_index = poll_file_descriptor_count++;
    poll_file_descriptors[start_monitor_poll_index] = (struct pollfd){
            .fd = start_monitor_timer_file_descriptor,
            .events = POLLIN,
            .revents = 0
    };

    int process_exit_poll_index = -1;
    if (process_exit_wait_file_descriptor >= 0) {
        process_exit_poll_index = poll_file_descriptor_count++;
        poll_file_descriptors[process_exit_poll_index] = (struct pollfd){
                .fd = process_exit_wait_file_descriptor,
                .events = POLLIN,
                .revents = 0
        };
    }

    int external_pid_fallback_poll_index = -1;
    if (external_pid_fallback_check_timer_file_descriptor >= 0) {
        external_pid_fallback_poll_index = poll_file_descriptor_count++;
        poll_file_descriptors[external_pid_fallback_poll_index] = (struct pollfd){
                .fd = external_pid_fallback_check_timer_file_descriptor,
                .events = POLLIN,
                .revents = 0
        };
    }
    //The child could exit after kill(pid, 0) succeeded but before SIGCHLD is delivered/observed
    exit_if_pid_has_finished(pid);

    if (debug) fprintf(stderr, "Wayland idle event loop started\n");
    while (1) {
        if (interruption_received) {
            result = handle_interruption();
            goto run_wayland_idle_event_loop_cleanup;
        }

        if (sigchld_received) {
            sigchld_received = 0;
            exit_if_pid_has_finished(pid);
        }

        if (wl_display_dispatch_pending(wayland_display) < 0) {
            if (errno == EINTR) {
                continue;
            }

            const int saved_errno = errno;
            fprintf_error("Wayland display dispatch_pending failed: %s\n", strerror(saved_errno));
            fprintf_error("User will be considered idle to allow the command to finish.\n");
            break;
        }

        if (wl_display_flush(wayland_display) < 0) {
            if (errno == EAGAIN) {
                wayland_flush_is_pending = 1;
            } else if (errno == EINTR) {
                continue;
            } else {
                const int saved_errno = errno;
                fprintf_error("Wayland display flush failed: %s\n", strerror(saved_errno));
                fprintf_error("User will be considered idle to allow the command to finish.\n");
                break;
            }
        } else {
            wayland_flush_is_pending = 0;
        }

        poll_file_descriptors[wayland_poll_index].events = POLLIN;
        if (wayland_flush_is_pending) {
            poll_file_descriptors[wayland_poll_index].events |= POLLOUT;
        }
        if (debug) {
            fprintf(stderr, "Wayland display file descriptor: %d, events: %d\n", wayland_display_file_descriptor,
                    poll_file_descriptors[wayland_poll_index].events);
            fprintf(stderr, "Start-monitor timer file descriptor: %d, events: %d\n",
                    start_monitor_timer_file_descriptor, poll_file_descriptors[start_monitor_poll_index].events);
            if (process_exit_poll_index >= 0) {
                fprintf(stderr, "Process-exit file descriptor: %d, events: %d\n", process_exit_wait_file_descriptor,
                        poll_file_descriptors[process_exit_poll_index].events);
            }
            if (external_pid_fallback_poll_index >= 0) {
                fprintf(stderr, "External-pid fallback timer file descriptor: %d, events: %d\n", external_pid_fallback_check_timer_file_descriptor,
                        poll_file_descriptors[external_pid_fallback_poll_index].events);
            }
        }
        const int poll_result = poll(poll_file_descriptors, poll_file_descriptor_count, -1);
        if (debug) fprintf(stderr, "poll() returned %d\n", poll_result);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            const int saved_errno = errno;
            fprintf_error("poll() failed: %s\n", strerror(saved_errno));
            result = -1;
            goto run_wayland_idle_event_loop_cleanup;
        }

        if (poll_file_descriptors[wayland_poll_index].revents & POLLNVAL) {
            fprintf_error("Wayland display file descriptor became invalid\n");
            result = -1;
            goto run_wayland_idle_event_loop_cleanup;
        }

        if (poll_file_descriptors[wayland_poll_index].revents & (POLLHUP | POLLERR)) {
            fprintf_error("Wayland connection closed, user will be considered idle to allow the command to finish.\n");
            break;
        }

        if (poll_file_descriptors[wayland_poll_index].revents & POLLOUT) {
            if (wl_display_flush(wayland_display) < 0) {
                if (errno == EAGAIN) {
                    wayland_flush_is_pending = 1;
                } else if (errno != EINTR) {
                    const int saved_errno = errno;
                    fprintf_error("Wayland display flush failed: %s\n", strerror(saved_errno));
                    fprintf_error("User will be considered idle to allow the command to finish.\n");
                    break;
                }
            } else {
                wayland_flush_is_pending = 0;
            }
        }

        if (!monitoring_started && poll_file_descriptors[start_monitor_poll_index].fd >= 0) {
            const short start_monitor_revents = poll_file_descriptors[start_monitor_poll_index].revents;

            if (start_monitor_revents & POLLNVAL) {
                fprintf_error("Start-monitor timer file descriptor became invalid\n");
                result = -1;
                goto run_wayland_idle_event_loop_cleanup;
            }

            if (start_monitor_revents & POLLERR) {
                fprintf_error("Start-monitor timer file descriptor reported an error\n");
                result = -1;
                goto run_wayland_idle_event_loop_cleanup;
            }

            if (start_monitor_revents & POLLIN) {
                if (consume_timer_file_descriptor_checked(start_monitor_timer_file_descriptor, "start-monitor") < 0) {
                    result = -1;
                    goto run_wayland_idle_event_loop_cleanup;
                }

                close_file_descriptor_if_open(&start_monitor_timer_file_descriptor, "start-monitor timer");
                poll_file_descriptors[start_monitor_poll_index].fd = -1;

                monitoring_started = 1;

                if (start_wayland_idle_notification_object(&wayland_idle_notification_listener) < 0) {
                    fprintf_error("Failed to create Wayland idle notification object, user will be considered idle.\n");
                    break;
                }

                if (!command_paused) {
                    pause_running_command_on_user_activity();
                }
            }
        }

        if (process_exit_poll_index >= 0) {
            const short process_exit_revents = poll_file_descriptors[process_exit_poll_index].revents;

            if (process_exit_revents & POLLNVAL) {
                fprintf_error("Process-exit file descriptor became invalid\n");
                result = -1;
                goto run_wayland_idle_event_loop_cleanup;
            }

            if (process_exit_revents & (POLLIN | POLLHUP | POLLERR)) {
                exit_if_pid_has_finished(pid);
            }
        }

        if (external_pid_fallback_poll_index >= 0) {
            const short fallback_revents = poll_file_descriptors[external_pid_fallback_poll_index].revents;

            if (fallback_revents & POLLNVAL) {
                fprintf_error("External-pid fallback timer file descriptor became invalid\n");
                result = -1;
                goto run_wayland_idle_event_loop_cleanup;
            }

            if (fallback_revents & POLLERR) {
                fprintf_error("External-pid fallback timer file descriptor reported an error\n");
                result = -1;
                goto run_wayland_idle_event_loop_cleanup;
            }

            if (fallback_revents & POLLIN) {
                if (consume_timer_file_descriptor_checked(external_pid_fallback_check_timer_file_descriptor,
                                                          "external-pid fallback") < 0) {
                    result = -1;
                    goto run_wayland_idle_event_loop_cleanup;
                }

                exit_if_pid_has_finished(pid);
            }
        }

        if (poll_file_descriptors[wayland_poll_index].revents & POLLIN) {
            const int dispatch_result = wl_display_dispatch(wayland_display);
            if (dispatch_result < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }

                const int saved_errno = errno;
                fprintf_error("Wayland display dispatch failed: %s\n", strerror(saved_errno));
                fprintf_error("User will be considered idle to allow the command to finish.\n");
                break;
            }
        }
    }

    close_file_descriptor_if_open(&start_monitor_timer_file_descriptor, "start-monitor timer");
    close_file_descriptor_if_open(&process_exit_wait_file_descriptor, "process-exit");
    close_file_descriptor_if_open(&external_pid_fallback_check_timer_file_descriptor, "external-pid fallback timer");
    if (verbose) {
        fprintf(stderr, "Wayland connection lost or loop finished.\n");
    }
    return resume_and_wait_for_pid_to_exit_checking_for_signals();

run_wayland_idle_event_loop_cleanup:
    close_file_descriptor_if_open(&start_monitor_timer_file_descriptor, "start-monitor timer");
    close_file_descriptor_if_open(&process_exit_wait_file_descriptor, "process-exit");
    close_file_descriptor_if_open(&external_pid_fallback_check_timer_file_descriptor, "external-pid fallback timer");

    return result;
}


static long long pause_or_resume_command_depending_on_user_activity(
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
            resume_paused_command_on_user_idle();
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
            pause_running_command_on_user_activity();
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

    best_effort_infer_graphical_session_environment_if_missing(verbose);

    const int wayland_loop_result = try_monitor_wayland_idle_notify(run_wayland_idle_event_loop);
    if (wayland_loop_result >= 0) {
        return wayland_loop_result;
    }

    //Wayland failed, try X11
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
        fprintf_error("No available method for detecting user idle time on the system, user will be considered always idle to allow the command to finish.\n");
    }

    struct timespec time_when_command_started;
    clock_gettime(CLOCK_MONOTONIC, &time_when_command_started);

    long long sleep_time_ms = POLLING_INTERVAL_BEFORE_STARTING_MONITORING_MS;
    unsigned long user_idle_time_ms = 0;

    if (verbose) {
        if (xscreensaver_is_available) {
            fprintf(stderr, "Starting to monitor user activity (X11 polling)\n");
        } else {
            fprintf(stderr, "Starting to monitor the process in fallback mode\n");
        }
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
