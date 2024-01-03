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
    struct ProcessNode *next_node;
} ProcessNode;

typedef struct ProcessInfo {
    int process_id;
    int parent_process_id;
} ProcessInfo;

ProcessNode *get_child_processes_linked_list(int initial_parent_process_id) {
    DIR *proc_directory = opendir("/proc/");
    if (proc_directory == NULL) {
        fprintf_error("Could not open /proc directory");
        exit(1);
    }

    // Stage 1: Read all process and parent IDs into an array
    const int NUMBER_OF_PROCESSES_INITIALLY_ALLOCATED = 4096;
    int processes_allocated = NUMBER_OF_PROCESSES_INITIALLY_ALLOCATED;
    ProcessInfo *all_processes = malloc(processes_allocated * sizeof(ProcessInfo));
    int total_processes = 0;

    struct dirent *directory_entry;
    const int STAT_FILE_PATH_MAX_LENGTH = 64; // proc/%d/stat, where max value of process_id is 4194304, so 64 should never be reached.
    char stat_file_path[STAT_FILE_PATH_MAX_LENGTH];
    FILE *stat_file;
    while ((directory_entry = readdir(proc_directory)) != NULL) {
        if (total_processes == processes_allocated) {
            processes_allocated *= 2;
            ProcessInfo *new_all_processes = realloc(all_processes, processes_allocated * sizeof(ProcessInfo));
            if (!new_all_processes) {
                perror("Failed to allocate memory while reading processes list");
                exit(1);
            }
            all_processes = new_all_processes;
        }
        int process_id, parent_process_id;

        //Skip everything that's not a directory
        if (directory_entry->d_type != DT_DIR) continue;

        //Skip all the dirs in that are not numbers
        if (sscanf(directory_entry->d_name, "%d", &process_id) != 1) continue;

        //Write path into stat_file_path
        snprintf(stat_file_path, sizeof(stat_file_path), "/proc/%d/stat", process_id);
        stat_file = fopen(stat_file_path, "r");
        if (stat_file == NULL) continue;

        //write parent PID into parent_process_id
        if (!fscanf(stat_file, "%*d %*s %*c %d", &parent_process_id)) {
            fprintf_error("Failed to parse %s\n", stat_file_path);
        }
        fclose(stat_file);

        all_processes[total_processes].process_id = process_id;
        all_processes[total_processes].parent_process_id = parent_process_id;
        total_processes++;
    }
    closedir(proc_directory);

    // Stage 2: Build a linked list containing only requested process and its children
    ProcessNode *head_node = NULL, **tail_node = &head_node;
    pid_t *descendants = malloc(sizeof(pid_t) * total_processes);
    if (descendants == NULL) {
        perror("Memory allocation failed");
        exit(1);
    }
    int known_descendants = 1; //initial value that will be increased if more descendants are found
    descendants[0] = initial_parent_process_id;

    int currently_checked_parent;
    //Iterations can be added to this loop when known_descendants is increased inside it.
    for (int descendantIndex = 0; descendantIndex < known_descendants; descendantIndex++) {
        currently_checked_parent = descendants[descendantIndex];
        for (int processIndex = 0; processIndex < total_processes; processIndex++) {
            if (all_processes[processIndex].parent_process_id != currently_checked_parent) continue;

            int new_process_id = all_processes[processIndex].process_id;

            // Add this process ID to descendants to check its children next.
            known_descendants++;
            descendants[known_descendants - 1] = new_process_id;

            // Create and append a new node
            ProcessNode *new_node = malloc(sizeof(ProcessNode));
            if (new_node == NULL) {
                perror("Memory allocation failed");
                exit(1);
            }
            new_node->process_id = new_process_id;
            new_node->next_node = NULL;
            *tail_node = new_node;
            tail_node = &(new_node->next_node);
        }
    }
    free(all_processes);
    free(descendants);
    return head_node;
}

void send_signal_to_pid(pid_t pid, int signal, char *signal_name) {
    if (debug) {
        printf("Sending %s to %i\n", signal_name, pid);
    }
    int kill_result = kill(pid, signal);
    if (kill_result == -1) {
        handle_kill_error(signal_name, pid, errno);
        exit(1);
    } else {
        if (debug) fprintf(stderr, "kill function sending %s returned %i\n", signal_name, kill_result);
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

void pause_command_recursively(pid_t pid) {
    pause_command(pid);
    ProcessNode *child_process_ids = get_child_processes_linked_list(pid);
    ProcessNode *current_node = child_process_ids;
    ProcessNode *previous_node;

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

void resume_command_recursively(pid_t pid) {
    resume_command(pid);
    ProcessNode *child_process_ids = get_child_processes_linked_list(pid);
    ProcessNode *current_node = child_process_ids;
    ProcessNode *previous_node;

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
