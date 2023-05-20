#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <X11/extensions/scrnsaver.h>
#include <getopt.h>

int verbose;
int quiet;

void handle_kill_error(char* signal_name, pid_t pid) {
    const char* reason;
    if (errno == EPERM) {
        reason = "Operation not permitted";
    } else if (errno == EINVAL) {
        reason = "Invalid signal number";
    } else if (errno == ESRCH) {
        reason = "No such process";
    }

    printf("Failed to send %s signal to PID %i: %s.\n", signal_name, pid, reason);
}

void pause_command(pid_t pid) {
    if (!quiet) {
        printf("User activity is detected, pausing PID %i.\n", pid);
    }
    if (kill(pid, SIGTSTP) == -1) {
        handle_kill_error("SIGCONT", pid);
        exit(1);
    }
}

void resume_command(pid_t pid) {
    if (!quiet) {
        printf("Lack of user activity is detected, resuming PID %i.\n", pid);
    }
    if (kill(pid, SIGCONT) == -1) {
        handle_kill_error("SIGCONT", pid);
        exit(1);
    }
}

void print_usage(char *binary_name) {
    printf("Usage: %s [--timeout|-t timeout_value_in_seconds] [--verbose|-v] [--quiet|-q] shell_command_to_run\n",
           binary_name);
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
        printf("Started \"%s\" with PID %i.\n",shell_command_to_run, pid);
    }
    return pid;
}

int main(int argc, char *argv[]) {
    pid_t pid;
    int user_idle_timeout_ms = 300000;

    // Define command line options
    struct option long_options[] = {
            {"timeout", required_argument, NULL, 't'},
            {"verbose", no_argument,       NULL, 'v'},
            {"quiet",   no_argument,       NULL, 'q'},
            {"help",    no_argument,       NULL, 'h'},
            {NULL,      0,                 NULL, 0}
    };

    // Parse command line options
    int option;
    while ((option = getopt_long(argc, argv, "hvqt:", long_options, NULL)) != -1) {
        switch (option) {
            case 't':
                user_idle_timeout_ms = atoi(optarg) * 1000;
                break;
            case 'v':
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
    if (optind >= argc) {
        print_usage(argv[0]);
        return 1;
    }
    if (quiet && verbose) {
        printf("Incompatible options --quiet|-q and --verbose|-v used");
        print_usage(argv[0]);
        return 1;
    }

    //Open display and initialize XScreensaverInfo for querying idle time
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Couldn't open an X11 display!\n");
        return 1;
    }
    XScreenSaverInfo *info = XScreenSaverAllocInfo();

    pid = run_shell_command(argv[optind], pid);

    int polling_interval_seconds = 1;
    int sleep_time_seconds = polling_interval_seconds;
    int command_paused = 0;
    // Monitor user activity
    while (1) {
        sleep(sleep_time_seconds);
        XScreenSaverQueryInfo(dpy, DefaultRootWindow(dpy), info);

        if (info->idle > user_idle_timeout_ms) {
            // User is inactive
            if (command_paused) {
                sleep_time_seconds = polling_interval_seconds; //reset to default value
                if (verbose) {
                    fprintf(stderr, "Idle time: %lums\n", info->idle);
                }

                resume_command(pid);
                command_paused = 0;
            }
        } else {
            // User is active
            if (!command_paused) {
                if (verbose) {
                    fprintf(stderr, "Idle time: %lums\n", info->idle);
                }
                pause_command(pid);
                command_paused = 1;
            }
            //TODO: this doesn't account for the time it took to pause the command
            sleep_time_seconds = ((user_idle_timeout_ms - info->idle) / 1000) - polling_interval_seconds;
            if (sleep_time_seconds < polling_interval_seconds) {
                sleep_time_seconds = polling_interval_seconds;
            }
            if (verbose) {
                fprintf(stderr,
                        "Polling every second is temporarily disabled due to user activity, next activity check scheduled in %u seconds\n",
                        sleep_time_seconds);
            }
        }

        // Check if the command has finished
        int status;
        if (waitpid(pid, &status, WNOHANG + WUNTRACED) == pid && WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            if (verbose) {
                fprintf(stderr, "Command has finished with exit code %u\n", exit_code);
            }
            return exit_code;
        }
    }
}
