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
 * Pauses a specified process by sending the SIGTSTP signal.
 *
 * @param pid The process ID of the target process.
 */
void pause_command(pid_t pid);

/**
 * Resumes a specified process by sending the SIGCONT signal.
 *
 * @param pid The process ID of the target process.
 */
void resume_command(pid_t pid);

/**
 * Executes a shell command in a new process and returns the process ID of the child process.
 * On failure will exit.
 *
 * @param shell_command_to_run The shell command to execute.
 * @return The PID of the child process on success,
 */
pid_t run_shell_command(const char *shell_command_to_run);
#endif //RUNWHENIDLE_PROCESS_HANDLING_H
