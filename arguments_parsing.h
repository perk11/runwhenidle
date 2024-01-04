#ifndef RUNWHENIDLE_ARGUMENTS_PARSING_H
#define RUNWHENIDLE_ARGUMENTS_PARSING_H
extern long start_monitor_after_ms;
extern long unsigned user_idle_timeout_ms;
extern char *shell_command_to_run;
extern pid_t external_pid;

/**
 * Parses command line arguments and sets relevant program options.
 *
 * @param argc The number of command line arguments.
 * @param argv An array of strings representing the command line arguments.
 */
void parse_command_line_arguments(int argc, char *argv[]);

#endif //RUNWHENIDLE_ARGUMENTS_PARSING_H
