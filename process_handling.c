#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#include "process_handling.h"
#include "output_settings.h"

pid_t run_shell_command(const char *shell_command_to_run) {
    if (verbose) {
        printf("Starting \"%s\"\n", shell_command_to_run);
    }
    pid_t pid = fork();
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

/**
 * Handles errors that may occur while sending a signal to a process.
 *
 * @param signal_name The name of the signal being sent.
 * @param pid         The process ID of the target process.
 */
void handle_kill_error(char *signal_name, pid_t pid) {
    const char *reason;
    if (errno == EPERM) {
        reason = "Operation not permitted";
    } else if (errno == EINVAL) {
        reason = "Invalid signal number";
    } else if (errno == ESRCH) {
        reason = "No such process";
    }

    fprintf(stderr, "Failed to send %s signal to PID %i: %s\n", signal_name, pid, reason);
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


int wait_for_pid_to_exit_synchronously(int pid) {
    int status;
    waitpid(pid, &status, 0);
    int exit_code = WEXITSTATUS(status);
    if (verbose) {
        fprintf(stderr, "PID %i has finished with exit code %u\n", pid, exit_code);
    }

    return exit_code;
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
