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
int xscreensaver_is_available;
Display *x_display;
XScreenSaverInfo *xscreensaver_info;
const long unsigned IDLE_TIME_NOT_AVAILABLE_VALUE = ULONG_MAX;

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

void pause_command(pid_t pid) {
    if (!quiet) {
        printf("User activity is detected, pausing PID %i\n", pid);
    }
    if (kill(pid, SIGTSTP) == -1) {
        handle_kill_error("SIGTSTP", pid);
        exit(1);
    }
}

void resume_command(pid_t pid) {
    if (!quiet) {
        printf("Lack of user activity is detected, resuming PID %i\n", pid);
    }
    if (kill(pid, SIGCONT) == -1) {
        handle_kill_error("SIGCONT", pid);
        exit(1);
    }
}

void print_usage(char *binary_name) {
    printf("Usage: %s [--timeout|-t timeout_value_in_seconds] [--verbose|-v] [--debug] [--quiet|-q] [--version|-V] shell_command_to_run [shell_command_arguments]\n",
           binary_name);
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
int main(int argc, char *argv[]) {
    pid_t pid;
    long unsigned user_idle_timeout_ms = 300000;

    // Define command line options
    struct option long_options[] = {
            {"timeout", required_argument, NULL, 't'},
            {"verbose", no_argument,       NULL, 'v'},
            {"debug", no_argument,         NULL, 'd'},
            {"quiet",   no_argument,       NULL, 'q'},
            {"help",    no_argument,       NULL, 'h'},
            {"version",    no_argument,    NULL, 'V'},
            {NULL, 0,                      NULL, 0}
    };

    // Parse command line options
    int option;
    while ((option = getopt_long(argc, argv, "+hvqt:V", long_options, NULL)) != -1) {
        switch (option) {
            case 't': {
                const long TIMEOUT_MAX_SUPPORTED_VALUE = 100000000; //~3 years
                const long TIMEOUT_MIN_SUPPORTED_VALUE = 1;
                long timeout_arg_value = strtol(optarg, NULL, 10);
                if (timeout_arg_value < TIMEOUT_MIN_SUPPORTED_VALUE ||
                    timeout_arg_value > TIMEOUT_MAX_SUPPORTED_VALUE || errno != 0) {
                    fprintf_error("Invalid timeout value: \"%s\". Range supported: %ld-%ld", optarg,
                                  TIMEOUT_MIN_SUPPORTED_VALUE, TIMEOUT_MAX_SUPPORTED_VALUE);
                    print_usage(argv[0]);
                    return 1;
                }
                user_idle_timeout_ms = timeout_arg_value * 1000;
                break;
            }
            case 'V':
                print_version();
                return 0;
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
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    if (debug) fprintf(stderr, "verbose: %i, debug: %i, quiet: %i, user_idle_timeout_ms: %i\n", verbose, debug, quiet, user_idle_timeout_ms);
    if (optind >= argc) {
        print_usage(argv[0]);
        return 1;
    }
    if (quiet && debug) {
        fprintf_error("Incompatible options --quiet|-q and --debug|-d used");
        print_usage(argv[0]);
        return 1;
    }
    if (quiet && verbose) {
        fprintf_error("Incompatible options --quiet|-q and --verbose|-v used");
        print_usage(argv[0]);
        return 1;
    }

    char *shell_command_to_run = read_remaining_arguments_as_char(argc, argv);

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

    // Let command run for 300ms to give it a chance to error-out or provide initial output.
    // 300ms is chosen to avoid giving user a noticeable delay while giving most quick commands a chance to finish.
    sleep_for_milliseconds(300);

    long long polling_interval_ms = 1000;
    long long sleep_time_ms = polling_interval_ms;
    int command_paused = 0;

    unsigned long user_idle_time_ms;
    // Monitor user activity
    while (1) {
        user_idle_time_ms = query_user_idle_time();

        // Checking this after querying the screensaver timer so that the command is still running while
        // we're querying the screensaver and has a chance to do some work and finish,
        // but before potentially pausing the command to avoid trying to pause it if it completed.
        exit_if_pid_has_finished(pid);

        if (user_idle_time_ms >= user_idle_timeout_ms) {
            if (debug) fprintf(stderr,"Idle time: %lums, idle timeout: %lums, user is inactive\n", user_idle_time_ms, user_idle_timeout_ms);
            if (command_paused) {
                sleep_time_ms = polling_interval_ms; //reset to default value
                if (verbose) {
                    fprintf(stderr, "Idle time: %lums, idle timeout: %lums, resuming command\n", user_idle_time_ms, user_idle_timeout_ms);
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
        if (debug) fprintf(stderr, "Sleeping for %lums\n", sleep_time_ms);
        sleep_for_milliseconds(sleep_time_ms);
    }
}
