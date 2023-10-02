#ifndef RUNWHENIDLE_PROCESS_HANDLING_H
#define RUNWHENIDLE_PROCESS_HANDLING_H
/**
 * Sends a signal to a specified process and handles any errors that occur during the process.
 *
 * @param pid         The process ID of the target process.
 * @param signal      The signal to send.
 * @param signal_name The name of the signal being sent.
 */
void send_signal_to_pid(pid_t pid, int signal, char *signal_name);
/**
 * Pauses a specified process using pause method specified in pause_method variable
 *
 * @param pid The process ID of the target process.
 */
void pause_command(pid_t pid);

/**
 * Pauses a specified process and all child processes
 *
 * @param pid The process ID of the target process.
 */
void pause_command_recursively(pid_t pid);


/**
 * Resumes a specified process by sending the SIGCONT signal.
 *
 * @param pid The process ID of the target process.
 */
void resume_command(pid_t pid);

/**
 * Resumes a specified process and all child processes by sending SIGCONT signal to each process.
 *
 * @param pid The process ID of the target process.
 */
void resume_command_recursively(pid_t pid);


/**
 * Executes a shell command in a new process and returns the process ID of the child process.
 * On failure will exit.
 *
 * @param shell_command_to_run The shell command to execute.
 * @return The PID of the child process on success,
 */
pid_t run_shell_command(const char *shell_command_to_run);

/**
 * Waits for a specific process to exit synchronously and returns its exit code.
 *
 * @param pid The process ID (PID) of the target process to wait for.
 * @return The exit code of the process
 */
int wait_for_pid_to_exit_synchronously(int pid);

/**
 * Checks if a specific process has finished and exits the current process with the same exit code if it has.
 * This function does not block and returns immediately if the process has not finished.
 *
 * @param pid The process ID (PID) of the target process to check.
 */
void exit_if_pid_has_finished(pid_t pid);
#endif //RUNWHENIDLE_PROCESS_HANDLING_H
