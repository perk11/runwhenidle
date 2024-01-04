#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>
#include <X11/extensions/scrnsaver.h>
#include <limits.h>

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
enum pause_method pause_method = PAUSE_METHOD_SIGTSTP;
long start_monitor_after_ms = 300;
long unsigned user_idle_timeout_ms = 300000;
long long polling_interval_ms = 1000;
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
            fprintf(stderr,
                    "Since command was previously paused, we will try to resume it now to be able to handle the interruption before exiting\n"
            );
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
    if (!quiet) {
        printf("Received SIGINT, sending SIGINT to the command and waiting for it to finish.\n");
    }
    send_signal_to_pid(pid, signum, "SIGINT");
    interruption_received = 1;
}

void sigterm_handler(int signum) {
    if (!quiet) {
        printf("Received SIGTERM, sending SIGTERM to the command and waiting for it to finish.\n");
    }
    send_signal_to_pid(pid, signum, "SIGTERM");
    interruption_received = 1;
}

long long pause_or_resume_command_depending_on_user_activity(
        long long polling_interval_ms,
        long long sleep_time_ms,
        unsigned long user_idle_time_ms) {
    if (user_idle_time_ms >= user_idle_timeout_ms) {
        if (debug)
            fprintf(stderr, "Idle time: %lums, idle timeout: %lums, user is inactive\n", user_idle_time_ms,
                    user_idle_timeout_ms);
        if (command_paused) {
            sleep_time_ms = polling_interval_ms; //reset to default value
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

        if (sleep_time_ms < polling_interval_ms) {
            if (debug)
                fprintf(stderr,
                        "Target sleep time %lldms is less than polling interval %lldms, resetting it to polling interval\n",
                        sleep_time_ms, polling_interval_ms);
            sleep_time_ms = polling_interval_ms;
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

    //Open display and initialize XScreensaverInfo for querying idle time
    x_display = XOpenDisplay(NULL);
    if (!x_display) {
        xscreensaver_is_available = 0;
        fprintf_error("Couldn't open an X11 display!\n");
    } else {
        int xscreensaver_event_base, xscreensaver_error_base; //not sure why these are needed
        xscreensaver_is_available = XScreenSaverQueryExtension(x_display, &xscreensaver_event_base,
                                                               &xscreensaver_error_base);
        if (xscreensaver_is_available) {
            xscreensaver_info = XScreenSaverAllocInfo();
        }
    }

    if (!xscreensaver_is_available) {
        fprintf_error(
                "No available method for detecting user idle time on the system, user will be considered idle to allow the command to finish.\n");
    }
    if (external_pid == 0) {
        pid = run_shell_command(shell_command_to_run);
    } else {
        pid = external_pid;
    }
    free(shell_command_to_run);
    struct timespec time_when_command_started;
    clock_gettime(CLOCK_MONOTONIC, &time_when_command_started);


    long long sleep_time_ms = POLLING_INTERVAL_BEFORE_STARTING_MONITORING_MS;
    unsigned long user_idle_time_ms = 0;
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigterm_handler);

    if (verbose) {
        fprintf(stderr, "Starting to monitor user activity\n");
    }
    // Monitor user activity
    while (1) {
        if (interruption_received) {
            return handle_interruption();
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
                    polling_interval_ms,
                    sleep_time_ms,
                    user_idle_time_ms);
        }
        if (debug) fprintf(stderr, "Sleeping for %lldms\n", sleep_time_ms);
        sleep_for_milliseconds(sleep_time_ms);
    }
}
