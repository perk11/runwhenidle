#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>

#include "arguments_parsing.h"
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

typedef struct ProcessInfo {
    int process_id;
    int parent_process_id;
} ProcessInfo;

pid_t read_parent_process_id(pid_t process_id) {
    const int STAT_FILE_PATH_MAX_LENGTH = 64; // proc/%d/stat, where max value of process_id is 4194304, so 64 should never be reached.
    char stat_file_path[STAT_FILE_PATH_MAX_LENGTH];
    //Write path into stat_file_path
    snprintf(stat_file_path, sizeof(stat_file_path), "/proc/%d/stat", process_id);

    // Examples of stat file contents:
    //3 (rcu_gp) I 2 0 0 0 -1 69238880 0 0 0 0 0 0 0 0 0 -20 1 0 40 0 0 18446744073709551615 0 0 0 0 0 0 0 2147483647 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0
    //2534 ((sd-pam)) S 2508 2508 2508 0 -1 4194624 56 0 0 0 0 0 0 0 20 0 1 0 1649 26865664 1392 18446744073709551615 1 1 0 0 0 0 0 4096 0 0 0 0 17 9 0 0 0 0 0 0 0 0 0 0 0 0 0
    //784178 (Isolated Web Co) S 3554906 3120 3120 0 -1 4194560 156270 0 0 0 563 133 0 0 20 0 26 0 78028739 2777669632 61094 18446744073709551615 94276324115952 94276324727360 140721125253344 0 0 0 0 69638 1082131704 0 0 0 17 19 0 0 0 0 0 94276324739952 94276324740056 94276339920896 140721125257544 140721125257859 140721125257859 140721125261279 0
    //87 (kworker/11:0H-events_highpri) I 2 0 0 0 -1 69238880 0 0 0 0 0 0 0 0 0 -20 1 0 41 0 0 18446744073709551615 0 0 0 0 0 0 0 2147483647 0 0 0 0 17 11 0 0 0 0 0 0 0 0 0 0 0 0 0

    // What we need is parent pid, which comes after state.
    // https://man7.org/linux/man-pages/man5/proc.5.html

    FILE *stat_file;
    stat_file = fopen(stat_file_path, "r");
    if (stat_file == NULL) {
        if (debug) {
            fprintf_error("Failed to open %s for reading\n", stat_file_path);
        }
        return 0;
    }
    const int MAX_STAT_FILE_READ_LENGTH =
            7 //length of 4194304 which is max PID value
            + 1 //space
            + 64 //Max length of "comm". Documentation says it's 16 characters, but I found longer examples. Better safe than sorry.
            + 2 //parenthesis around comm
            + 1 //space
            + 1 //state
            + 1 //space
            + 7 //length of 4194304
            + 1 //space
    ;
    char file_contents[MAX_STAT_FILE_READ_LENGTH];
    if (!fgets(file_contents, MAX_STAT_FILE_READ_LENGTH, stat_file)) {
        fprintf_error("Failed to read from %s\n", stat_file);
        return 0;
    }
    fclose(stat_file);
    const int MIN_STAT_FILE_READ_CLOSING_PARENTHESIS_POSITION =
            1 //min PID length
            + 1//space
            + 1 //opening parenthesis
    ;
    int file_contents_index;
    //loop until we find ") ".
    for (file_contents_index = MIN_STAT_FILE_READ_CLOSING_PARENTHESIS_POSITION;
         file_contents_index < MAX_STAT_FILE_READ_LENGTH - 1; file_contents_index++) {
        if (file_contents[file_contents_index] == ')' && file_contents[file_contents_index + 1] == ' ') {
            break;
        }
    }
    if (file_contents_index == MAX_STAT_FILE_READ_LENGTH - 1) {
        fprintf_error("Failed to parse %s: reached %d bytes and but did not find \") \".\n", stat_file_path,
                      MAX_STAT_FILE_READ_LENGTH);
        return 0;
    }
    char *parent_process_string = strtok(&file_contents[file_contents_index + 3], " ");
    return strtol(parent_process_string, NULL, 10);
}

ProcessInfo *get_child_processes(int initial_parent_process_id) {
    DIR *proc_directory = opendir("/proc/");
    if (proc_directory == NULL) {
        fprintf_error("Could not open /proc directory");
        exit(1);
    }

    // Stage 1: Read all process and parent IDs into an array
    const int NUMBER_OF_PROCESSES_INITIALLY_ALLOCATED = 4096;
    int processes_allocated = NUMBER_OF_PROCESSES_INITIALLY_ALLOCATED;
    ProcessInfo *all_processes;
    all_processes = malloc(processes_allocated * (sizeof *all_processes));
    int total_processes = 0;

    struct dirent *directory_entry;

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

        parent_process_id = read_parent_process_id(process_id);
        if (parent_process_id == 0) {
            if (debug) {
                fprintf_error("Failed to read parent process id for %d\n", process_id);
            }
            continue;
        }

        all_processes[total_processes].process_id = process_id;
        all_processes[total_processes].parent_process_id = parent_process_id;
        total_processes++;
    }
    closedir(proc_directory);

    // Stage 2: Build an array containing only children of a process
    ProcessInfo *descendants;
    descendants = malloc((sizeof *descendants) * total_processes);
    if (descendants == NULL) {
        perror("Memory allocation failed");
        exit(1);
    }
    int known_descendants = 1; //initial value that will be increased if more descendants are found
    pid_t checked_process_id = initial_parent_process_id;

    //Iterations can be added to this loop when known_descendants is increased inside it.
    for (int descendantIndex = 0; descendantIndex < known_descendants; descendantIndex++) {
        for (int processIndex = 0; processIndex < total_processes; processIndex++) {
            if (all_processes[processIndex].parent_process_id != checked_process_id) continue;

            // Add this process ID to descendants to check its children next.
            descendants[known_descendants - 1] = all_processes[processIndex];
            known_descendants++;
        }
        checked_process_id = descendants[descendantIndex].process_id;
    }
    descendants[known_descendants - 1].process_id = 0;
    free(all_processes);

    return descendants;
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
    ProcessInfo *child_process_ids = get_child_processes(pid);
    ProcessInfo *initial_child_process_ids_pointer = child_process_ids;
    while (child_process_ids->process_id != 0) {
        pause_command(child_process_ids->process_id);
        child_process_ids++;
    }
    free(initial_child_process_ids_pointer);
}

void resume_command(pid_t pid) {
    if (!quiet) {
        printf("Resuming PID %i\n", pid);
    }
    send_signal_to_pid(pid, SIGCONT, "SIGCONT");
}

void resume_command_recursively(pid_t pid) {
    resume_command(pid);
    ProcessInfo *child_process_ids = get_child_processes(pid);
    ProcessInfo *initial_child_process_ids_pointer = child_process_ids;
    while (child_process_ids->process_id != 0) {
        resume_command(child_process_ids->process_id);
        child_process_ids++;
    }
    free(initial_child_process_ids_pointer);
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
    int finished = 0;
    int exit_code;
    if (debug) fprintf(stderr, "Checking if PID %i has finished\n", pid);
    if (external_pid) {
        if (kill(pid, 0) == -1) {
            finished = 1;
            exit_code = 0;
        }
    } else {
        if (waitpid(pid, &status, WNOHANG + WUNTRACED) == pid && WIFEXITED(status)) {
            finished = 1;
            exit_code = WEXITSTATUS(status);
        }
    }

    if (finished) {
        if (verbose) {
            fprintf(stderr, "PID %i has finished", pid);
            if (!external_pid) {
                fprintf(stderr, " with exit code %u", exit_code);
            }
            fprintf(stderr, "\n");
        }
        exit(exit_code);
    }
}
