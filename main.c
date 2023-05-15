#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <X11/extensions/scrnsaver.h>
#include <getopt.h>

void pause_command(pid_t pid) {
    printf("Pausing command...\n");
    if (kill(pid, SIGTSTP) == -1) {
        printf("Pause didn't complete: command has already finished.\n");
        exit(1);
    }
}

void resume_command(pid_t pid) {
    printf("Resuming command...\n");
    if (kill(pid, SIGCONT) == -1) {
        printf("Resume didn't complete: command has already finished.\n");
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    const char *shell_command_to_run = NULL;
    pid_t pid;
    int user_idle_timeout_ms = 300000;

    // Define command line options
    struct option long_options[] = {
            {"timeout", required_argument, NULL, 't'},
            {NULL, 0,                      NULL, 0}
    };

    // Parse command line options
    int option;
    while ((option = getopt_long(argc, argv, "t:", long_options, NULL)) != -1) {
        switch (option) {
            case 't':
                user_idle_timeout_ms = atoi(optarg);
                break;
            default:
                printf("Usage: %s [--timeout timeout] shell_command_to_run\n", argv[0]);
                return 1;
        }
    }
    if (optind >= argc) {
        printf("Usage: %s [--timeout timeout] shell_command_to_run\n", argv[0]);
        return 1;
    }

    shell_command_to_run = argv[optind];

    // Fork a child process to run the command
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

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        printf("Couldn't open an X11 display!\n");
        return 1;
    }

    XScreenSaverInfo *info = XScreenSaverAllocInfo();
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
                fprintf(stderr, "Idle time: %lums\n", info->idle);

                resume_command(pid);
                command_paused = 0;
            }
        } else {
            // User is active
            if (!command_paused) {
                fprintf(stderr, "Idle time: %lums\n", info->idle);
                pause_command(pid);
                command_paused = 1;
            }
            //TODO: this doesn't account for the time it took to pause the command
            sleep_time_seconds = ((user_idle_timeout_ms - info->idle) / 1000) - polling_interval_seconds;
            if (sleep_time_seconds < polling_interval_seconds) {
                sleep_time_seconds = polling_interval_seconds;
            }
            fprintf(stderr, "User is active, we will check again in %u seconds\n", sleep_time_seconds);
        }

        // Check if the command has finished
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid && WIFEXITED(status)) {
            fprintf(stderr, "Command has finished\n");
            return status;
        }
    }
}
