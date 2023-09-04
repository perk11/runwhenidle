#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/wait.h>
#include <X11/extensions/scrnsaver.h>
#include <getopt.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#include "sleep_utils.h"
#include "time_utils.h"
#include "tty_utils.h"

#ifndef VERSION
#define VERSION 'unkown'
#endif

int verbose = 0;
int quiet = 0;
int debug = 0;
int monitoring_started = 0;
long start_monitor_after_ms = 300;
long unsigned user_idle_timeout_ms = 300000;
long long polling_interval_ms = 1000;
const long long POLLING_INTERVAL_BEFORE_STARTING_MONITORING_MS = 100;
const long TIMEOUT_MAX_SUPPORTED_VALUE = 100000000; //~3 years
const long TIMEOUT_MIN_SUPPORTED_VALUE = 1;
const long START_MONITOR_AFTER_MAX_SUPPORTED_VALUE = TIMEOUT_MAX_SUPPORTED_VALUE*1000;
const long START_MONITOR_AFTER_MIN_SUPPORTED_VALUE = 0;

int xscreensaver_is_available;
Display *x_display;
XScreenSaverInfo *xscreensaver_info;
const long unsigned IDLE_TIME_NOT_AVAILABLE_VALUE = ULONG_MAX;

volatile sig_atomic_t interruption_received = 0;
volatile sig_atomic_t command_paused = 0;
pid_t pid;

void handle_kill_error(char *signal_name, pid_t pid) {
    const char *reason;
    if (errno == EPERM) {
        reason = "Operation not permitted";
    } else if (errno == EINVAL) {
        reason = "Invalid signal number";
    } else if (errno == ESRCH) {
        reason = "No such process";
    }

    printf("Failed to send %s signal to PID %i: %s\n", signal_name, pid, reason);
}
void send_signal_to_pid(pid_t pid, int signal, char *signal_name) {
    if (debug) {
        printf("Sending %s to %i\n",signal_name, pid);
    }
    int kill_result = kill(pid, signal);
    if (kill_result == -1) {
        handle_kill_error(signal_name, pid);
        exit(1);
    } else {
        if (debug) fprintf(stderr, "kill function sending %s returned %i\n",signal_name, kill_result);
    }
}

void pause_command(pid_t pid) {
    if (!quiet) {
        printf("User activity is detected, pausing PID %i\n", pid);
    }
    send_signal_to_pid(pid, SIGTSTP, "SIGTSTP");
}

void resume_command(pid_t pid) {
    if (!quiet) {
        printf("Resuming PID %i\n", pid);
    }
    send_signal_to_pid(pid, SIGCONT, "SIGCONT");
}

void print_usage(char *binary_name) {
    printf("Usage: %s [OPTIONS] shell_command_to_run [shell_command_arguments]\n", binary_name);
    printf("\nOptions:\n");
    printf("  --timeout, -t <timeout_value_in_seconds>  Set the user idle time after which the command can run in seconds (default: 300 seconds).\n");
    printf("  --start-monitor-after, -a <delay_in_ms>   Set an initial delay in milliseconds before monitoring starts. During this time command runs unrestricted. This helps to catch quick errors. (default: 300 ms).\n");
    printf("  --verbose, -v                             Enable verbose output for monitoring.\n");
    printf("  --debug                                   Enable debugging output.\n");
    printf("  --quiet, -q                               Suppress all program output except errors.\n");
    printf("  --version, -V                             Print the program version information.\n");
}
void print_version() {
    printf("runwhenidle %s\n", VERSION);
}

pid_t run_shell_command(const char *shell_command_to_run, pid_t pid) {
    if (verbose) {
        printf("Starting \"%s\"\n", shell_command_to_run);
    }
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    } else if (pid == 0) {
        // Child process
        execl("/bin/sh", "sh", "-c", shell_command_to_run, (char *) NULL);
        perror("execl");
        exit(1);
    }
    if (!quiet) {
        printf("Started \"%s\" with PID %i\n", shell_command_to_run, pid);
    }
    return pid;
}

void exit_if_pid_has_finished(pid_t pid) {
    int status;
    if (debug) fprintf(stderr, "Checking if PID %i has finished\n", pid);
    if (waitpid(pid, &status, WNOHANG + WUNTRACED) == pid && WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (verbose) {
            fprintf(stderr, "PID %i has finished with exit code %u\n", pid, exit_code);
        }
        exit(exit_code);
    }
}

char *read_remaining_arguments_as_char(int argc,
                                       char *const *argv) {
    if (optind == argc) { //there is one argument remaining
        char *last_and_only_argument = strdup(argv[optind]);
        return last_and_only_argument;
    }

    size_t memory_to_be_allocated_for_remaining_arguments_string = 0;
    for (int i = optind; i < argc; i++) {
        memory_to_be_allocated_for_remaining_arguments_string +=
                strlen(argv[i]) + 1; // +1 for space separator or null terminator
    }

    char *remaining_arguments_string = NULL; // Variable to store the remaining_arguments_string

    // Allocate memory for the remaining_arguments_string
    remaining_arguments_string = malloc(memory_to_be_allocated_for_remaining_arguments_string);
    if (remaining_arguments_string == NULL) {
        //not using fprintf_error here intentionally
        fprintf(stderr, "Failed to allocate memory while parsing command to be ran.\n");
        exit(1);
    }

    size_t current_length_of_all_arguments = 0;
    for (int i = optind; i < argc; i++) {
        size_t current_argument_length = strlen(argv[i]);
        memcpy(remaining_arguments_string + current_length_of_all_arguments, argv[i], current_argument_length);
        current_length_of_all_arguments += current_argument_length;
        remaining_arguments_string[current_length_of_all_arguments++] = ' '; // Add space separator
    }
    assert(current_length_of_all_arguments == memory_to_be_allocated_for_remaining_arguments_string);
    remaining_arguments_string[current_length_of_all_arguments - 1] = '\0'; // Replace the last space separator with a null terminator

    return remaining_arguments_string;
}
long unsigned query_user_idle_time()
{
    if (xscreensaver_is_available) {
        XScreenSaverQueryInfo(x_display, DefaultRootWindow(x_display), xscreensaver_info);
        return xscreensaver_info->idle;
    }

    return IDLE_TIME_NOT_AVAILABLE_VALUE;
}
int wait_for_pid_to_exit_synchronously(int pid) {
    int status;
    waitpid(pid, &status, 0);
    int exit_code = WEXITSTATUS(status);
    if (verbose) {
        fprintf(stderr, "PID %i has finished with exit code %u\n", pid, exit_code);
    }

    return exit_code;
}
int handle_interruption() {
    if (command_paused) {
        if (verbose) {
            fprintf(stderr,
                    "Since command was previously paused, we will try to resume it now to be able to handle the interruption before exiting\n"
            );
        }
        resume_command(pid);
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
        if (debug) fprintf(stderr,"Idle time: %lums, idle timeout: %lums, user is inactive\n", user_idle_time_ms, user_idle_timeout_ms);
        if (command_paused) {
            sleep_time_ms = polling_interval_ms; //reset to default value
            if (verbose) {
                fprintf(stderr, "Idle time: %lums, idle timeout: %lums, resuming command\n", user_idle_time_ms, user_idle_timeout_ms);
            }
            if (!quiet){
                printf("Lack of user activity detected. ");
                //intentionally no new line here, resume_command will print the rest of the message.
            }
            resume_command(pid);
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
            pause_command(pid);
            if (debug) fprintf(stderr,"Command paused\n");
            command_paused = 1;
            command_was_paused_this_iteration = 1;
        }
        sleep_time_ms = user_idle_timeout_ms - user_idle_time_ms;
        if (debug) fprintf(stderr,"Target sleep time: %lums\n", sleep_time_ms);
        if (command_was_paused_this_iteration) {
            if (debug) fprintf(stderr, "Command was paused this iteration\n");
            struct timespec time_before_sleep;
            clock_gettime(CLOCK_MONOTONIC, &time_before_sleep);
            long long pausing_time_ms = get_elapsed_time_ms(time_when_starting_to_pause, time_before_sleep);
            if (debug) fprintf(stderr, "Target sleep time before taking into account time it took to pause: %lums, time it took to pause: %lums\n", sleep_time_ms, pausing_time_ms);
            sleep_time_ms = sleep_time_ms - pausing_time_ms;

        }

        if (sleep_time_ms < polling_interval_ms) {
            if (debug) fprintf(stderr, "Target sleep time %lums is less than polling interval %lums, resetting it to polling interval\n", sleep_time_ms, polling_interval_ms);
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

char *parse_command_line_arguments(int argc, char *argv[]) {// Define command line options
    struct option long_options[] = {
            {"timeout", required_argument, NULL, 't'},
            {"start-monitor-after", required_argument, NULL, 'a'},
            {"verbose", no_argument,       NULL, 'v'},
            {"debug", no_argument,         NULL, 'd'},
            {"quiet",   no_argument,       NULL, 'q'},
            {"help",    no_argument,       NULL, 'h'},
            {"version",    no_argument,    NULL, 'V'},
            {NULL, 0,                      NULL, 0}
    };

    // Parse command line options
    int option;
    while ((option = getopt_long(argc, argv, "+hvqt:a:V", long_options, NULL)) != -1) {
        switch (option) {
            case 't': {

                long timeout_arg_value = strtol(optarg, NULL, 10);
                if (timeout_arg_value < TIMEOUT_MIN_SUPPORTED_VALUE ||
                    timeout_arg_value > TIMEOUT_MAX_SUPPORTED_VALUE || errno != 0) {
                    fprintf_error("Invalid timeout value: \"%s\". Range supported: %ld-%ld", optarg,
                                  TIMEOUT_MIN_SUPPORTED_VALUE, TIMEOUT_MAX_SUPPORTED_VALUE);
                    print_usage(argv[0]);
                    exit(1);
                }
                user_idle_timeout_ms = timeout_arg_value * 1000;
                break;
            }
            case 'a': {
                start_monitor_after_ms = strtol(optarg, NULL, 10);

                if (start_monitor_after_ms < START_MONITOR_AFTER_MIN_SUPPORTED_VALUE || errno != 0) {
                    fprintf_error( "Invalid start-monitor-after time value: \"%s\" Range supported: %ld-%ld.\n", optarg,
                            START_MONITOR_AFTER_MIN_SUPPORTED_VALUE, START_MONITOR_AFTER_MAX_SUPPORTED_VALUE
                    );
                    print_usage(argv[0]);
                    exit(1);
                }
                break;
            }
            case 'V':
                print_version();
                exit(0);
            case 'v':
                verbose = 1;
                break;
            case 'd':
                debug = 1;
                verbose = 1;
                break;
            case 'q':
                quiet = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
                break;
            default:
                print_usage(argv[0]);
                exit(1);
        }
    }
    if (debug) fprintf(stderr, "verbose: %i, debug: %i, quiet: %i, user_idle_timeout_ms: %i, start_monitoring_after_ms: %lld\n", verbose, debug, quiet, user_idle_timeout_ms, start_monitor_after_ms);
    if (optind >= argc) {
        print_usage(argv[0]);
        exit(1);
    }
    if (quiet && debug) {
        fprintf_error("Incompatible options --quiet|-q and --debug used");
        print_usage(argv[0]);
        exit(1);
    }
    if (quiet && verbose) {
        fprintf_error("Incompatible options --quiet|-q and --verbose|-v used");
        print_usage(argv[0]);
        exit(1);
    }

    return read_remaining_arguments_as_char(argc, argv);
}

int main(int argc, char *argv[]) {
    char *shell_command_to_run = parse_command_line_arguments(argc, argv);

    //Open display and initialize XScreensaverInfo for querying idle time
    x_display = XOpenDisplay(NULL);
    if (!x_display) {
        xscreensaver_is_available = 0;
        fprintf_error("Couldn't open an X11 display!");
    } else {
        int xscreensaver_event_base, xscreensaver_error_base; //not sure why these are neeeded
        xscreensaver_is_available = XScreenSaverQueryExtension(x_display, &xscreensaver_event_base,
                                                               &xscreensaver_error_base);
        if (xscreensaver_is_available) {
            xscreensaver_info = XScreenSaverAllocInfo();
        }
    }

    if (!xscreensaver_is_available) {
        fprintf_error("No available method for detecting user idle time on the system, user will be considered idle to allow the command to finish.");
    }

    pid = run_shell_command(shell_command_to_run, pid);
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
        if (debug) fprintf(stderr, "Sleeping for %lums\n", sleep_time_ms);
        sleep_for_milliseconds(sleep_time_ms);
    }
}
