#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>

#include "process_handling.h"
#include "output_settings.h"
#include "pause_methods.h"
#include "tty_utils.h"

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
 * @param kill_errno   errno of kill function
 */
void handle_kill_error(char *signal_name, pid_t pid, int kill_errno) {
    fprintf(stderr, "Failed to send %s signal to PID %i: %s\n", signal_name, pid, strerror(kill_errno));
}
typedef struct ProcessNode {
    int process_id;
    struct ProcessNode* next_node;
} ProcessNode;


ProcessNode* get_child_processes_linked_list(int parent_process_id) {
    DIR *proc_directory = opendir("/proc/");
    if (proc_directory == NULL) {
        fprintf_error("Could not open /proc directory");
        exit(1);
    }

    ProcessNode *head_node = NULL, **tail_node = &head_node;
    struct dirent *directory_entry;
    char stat_file_path[64];
    int process_id, found_parent_process_id;
    FILE *stat_file;

    while ((directory_entry = readdir(proc_directory)) != NULL) {
        if (sscanf(directory_entry->d_name, "%d", &process_id) != 1) continue;

        snprintf(stat_file_path, sizeof(stat_file_path), "/proc/%d/stat", process_id);
        stat_file = fopen(stat_file_path, "r");
        if (stat_file == NULL) continue;

        fscanf(stat_file, "%*d %*s %*c %d", &found_parent_process_id);
        fclose(stat_file);

        if (found_parent_process_id != parent_process_id) continue;

        ProcessNode *new_node = (ProcessNode *)malloc(sizeof(ProcessNode));
        if (new_node == NULL) {
            perror("Memory allocation failed");
            exit(1);
        }

        new_node->process_id = process_id;
        new_node->next_node = NULL;
        *tail_node = new_node;
        tail_node = &(new_node->next_node);
    }

    closedir(proc_directory);
    return head_node;
}

void send_signal_to_pid(pid_t pid, int signal, char *signal_name) {
    if (debug) {
        printf("Sending %s to %i\n",signal_name, pid);
    }
    int kill_result = kill(pid, signal);
    if (kill_result == -1) {
        handle_kill_error(signal_name, pid, errno);
        exit(1);
    } else {
        if (debug) fprintf(stderr, "kill function sending %s returned %i\n",signal_name, kill_result);
    }
}

void pause_command(pid_t pid) {
    if (!quiet) {
        printf("Pausing PID %i\n", pid);
    }
    switch (pause_method) {
        case PAUSE_METHOD_SIGTSTP:
            send_signal_to_pid(pid, SIGTSTP, "SIGTSTP");
            break;
        case PAUSE_METHOD_SIGSTOP:
            send_signal_to_pid(pid, SIGSTOP, "SIGSTOP");
            break;
        default:
            fprintf_error("Unsupported pause method: %i\n", pause_method);
            exit(1);
    }
}

void pause_command_recursively(pid_t pid)
{
    pause_command(pid);
    ProcessNode* child_process_ids = get_child_processes_linked_list(pid);
    ProcessNode* current_node = child_process_ids;
    ProcessNode* previous_node;

    while (current_node) {
        pause_command(current_node->process_id);
        previous_node = current_node;
        current_node = current_node->next_node;
        free(previous_node);
    }
}

void resume_command(pid_t pid) {
    if (!quiet) {
        printf("Resuming PID %i\n", pid);
    }
    send_signal_to_pid(pid, SIGCONT, "SIGCONT");
}

void resume_command_recursively(pid_t pid)
{
    resume_command(pid);
    ProcessNode* child_process_ids = get_child_processes_linked_list(pid);
    ProcessNode* current_node = child_process_ids;
    ProcessNode* previous_node;

    while (current_node) {
        resume_command(current_node->process_id);
        previous_node = current_node;
        current_node = current_node->next_node;
        free(previous_node);
    }
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
