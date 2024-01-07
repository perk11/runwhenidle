#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>

#include "output_settings.h"
#include "arguments_parsing.h"
#include "tty_utils.h"
#include "pause_methods.h"

const long TIMEOUT_MAX_SUPPORTED_VALUE = 100000000; //~3 years
const long TIMEOUT_MIN_SUPPORTED_VALUE = 1;
const long START_MONITOR_AFTER_MAX_SUPPORTED_VALUE = TIMEOUT_MAX_SUPPORTED_VALUE * 1000;
const long START_MONITOR_AFTER_MIN_SUPPORTED_VALUE = 0;


void print_usage(char *binary_name) {
    printf("Usage: %s [OPTIONS] [shell_command_to_run] [shell_command_arguments]\n", binary_name);
    printf("\nOptions:\n");
    printf("  --timeout|-t <seconds>        Set the user idle time after which the process\n"
           "                                can be resumed in seconds. (default: 300 seconds).\n\n");
    printf("  --pid|-p <pid>                Monitor an existing process. When this option is used,\n"
           "                                shell_command_to_run should not be passed.\n\n");
    printf("  --start-monitor-after|-a <ms> Set an initial delay in milliseconds before monitoring\n"
           "                                starts. During this time the process runs unrestricted.\n"
           "                                This helps to catch quick errors. (default: 300 ms).\n\n");
    printf("  --pause-method|-m <method>    Specify method for pausing the process when the user is\n"
           "                                not idle. Available Options (default: SIGSTOP): \n"
           "                                    SIGTSTP (can be ignored by the program),\n"
           "                                    SIGSTOP (cannot be ignored).\n\n");
    printf("  --quiet|-q                    Suppress all output from %s except errors and only\n"
           "                                display output from the command that is running.\n"
           "                                No output if --pid options is used.\n\n", binary_name);
    printf("  --verbose|-v                  Enable verbose output for monitoring.\n");
    printf("  --debug                       Enable debugging output.\n");
    printf("  --version|-V                  Print the program version information.\n");
}

void print_version() {
    printf("runwhenidle %s\n", VERSION);
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
    remaining_arguments_string[current_length_of_all_arguments -
                               1] = '\0'; // Replace the last space separator with a null terminator

    return remaining_arguments_string;
}

void print_buffered_error_and_restore_stderr(FILE *old_stderr, const char *getopt_error_buffer, int buffer_size) {
    fflush(stderr);
    fclose(stderr);
    stderr = old_stderr;
    if (getopt_error_buffer[0] != '\0') {
        fprintf_error("%s", getopt_error_buffer);
        if (getopt_error_buffer[buffer_size - 2] != '\0') {
            fprintf_error(" (error message truncated)\n");
        }
    }
}

void parse_command_line_arguments(int argc, char *argv[]) {
    struct option long_options[] = {
            {"timeout",             required_argument, NULL, 't'},
            {"pid",                 required_argument, NULL, 'p'},
            {"start-monitor-after", required_argument, NULL, 'a'},
            {"pause-method",        required_argument, NULL, 'm'},
            {"verbose",             no_argument,       NULL, 'v'},
            {"debug",               no_argument,       NULL, 'd'},
            {"quiet",               no_argument,       NULL, 'q'},
            {"help",                no_argument,       NULL, 'h'},
            {"version",             no_argument,       NULL, 'V'},
            {NULL, 0,                                  NULL, 0}
    };

    // Redirect stderr to a buffer to capture getopt errors
    FILE *old_stderr;
    char getopt_error_buffer[1024] = {0};
    fflush(stderr);
    old_stderr = stderr;
    stderr = fmemopen(getopt_error_buffer, sizeof(getopt_error_buffer), "w");;

    // Parse command line options
    int option;
    while ((option = getopt_long(argc, argv, "+hvqp:t:a:m:V", long_options, NULL)) != -1) {
        switch (option) {
            case 't': {
                char *strtol_endptr;
                long timeout_arg_value = strtol(optarg, &strtol_endptr, 10);
                if (timeout_arg_value < TIMEOUT_MIN_SUPPORTED_VALUE ||
                    timeout_arg_value > TIMEOUT_MAX_SUPPORTED_VALUE || errno != 0 || *strtol_endptr != '\0') {
                    print_buffered_error_and_restore_stderr(old_stderr, getopt_error_buffer, sizeof (getopt_error_buffer));
                    fprintf_error("%s: Invalid timeout value: \"%s\". Range supported: %ld-%ld\n",
                                  argv[0],
                                  optarg,
                                  TIMEOUT_MIN_SUPPORTED_VALUE, TIMEOUT_MAX_SUPPORTED_VALUE);
                    exit(1);
                }
                user_idle_timeout_ms = timeout_arg_value * 1000;
                break;
            }
            case 'p': {
                char *strtol_endptr;
                external_pid = strtol(optarg, &strtol_endptr, 10);
                if (external_pid < 1 || errno != 0 || *strtol_endptr != '\0') {
                    print_buffered_error_and_restore_stderr(old_stderr, getopt_error_buffer, sizeof (getopt_error_buffer));
                    fprintf_error("%s: Invalid pid value: \"%s\"\n", argv[0], optarg);
                    exit(1);
                }

                break;
            }
            case 'a': {
                char *strtol_endptr;
                start_monitor_after_ms = strtol(optarg, &strtol_endptr, 10);

                if (start_monitor_after_ms < START_MONITOR_AFTER_MIN_SUPPORTED_VALUE || errno != 0 || *strtol_endptr != '\0') {
                    print_buffered_error_and_restore_stderr(old_stderr, getopt_error_buffer, sizeof (getopt_error_buffer));
                    fprintf_error("%s: Invalid start-monitor-after time value: \"%s\" Range supported: %ld-%ld.\n",
                                  argv[0],
                                  optarg,
                                  START_MONITOR_AFTER_MIN_SUPPORTED_VALUE, START_MONITOR_AFTER_MAX_SUPPORTED_VALUE
                    );
                    exit(1);
                }
                break;
                }
            case 'm': {
                char *method = strdup(optarg);
                for (int i = 0; i < sizeof(method); i++) {
                    method[i] = toupper(method[i]);
                }
                pause_method = PAUSE_METHOD_UNKNOWN;
                for (int i = 1; pause_method_string[i] != NULL; i++) {
                    if (strcmp(pause_method_string[i], method) == 0) {
                        pause_method = i;
                        break;
                    }
                }
                if (pause_method == PAUSE_METHOD_UNKNOWN) {
                    print_buffered_error_and_restore_stderr(old_stderr, getopt_error_buffer, sizeof (getopt_error_buffer));
                    fprintf_error("%s: Invalid value for --pause-method|m argument: \"%s\". Supported values: ",
                                  argv[0],
                                  optarg
                    );
                    for (int i = 1; pause_method_string[i] != NULL; i++) {
                        fprintf_error(pause_method_string[i]);
                        if (pause_method_string[i + 1] != NULL) {
                            fprintf_error(", ");
                        }
                    }
                    fprintf_error("\n");
                    exit(1);
                }
                break;
            }
            case 'V':
                print_version();
                exit(0);
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
                print_usage(argv[0]);
                exit(0);
            default:
                print_buffered_error_and_restore_stderr(old_stderr, getopt_error_buffer, sizeof (getopt_error_buffer));
                fprintf(stderr, "Try %s --help for more information\n", argv[0]);
                exit(1);
        }
    }
    print_buffered_error_and_restore_stderr(old_stderr, getopt_error_buffer, sizeof (getopt_error_buffer));

    if (debug)
        fprintf(stderr,
                "verbose: %i, debug: %i, quiet: %i, pause_method: %i, user_idle_timeout_ms: %lu, start_monitoring_after_ms: %ld\n",
                verbose,
                debug,
                quiet,
                pause_method,
                user_idle_timeout_ms,
                start_monitor_after_ms
        );
    if (external_pid) {
        if (optind < argc) {
            fprintf_error(
                    "%s: Running command is not supported when -p option is used. Found unexpected \"%s\"\n",
                    argv[0],
                    read_remaining_arguments_as_char(argc, argv)
            );
            fprintf(stderr, "Try %s --help for more information\n", argv[0]);
            exit(1);
        }
    } else {
        if (optind >= argc) {
            fprintf_error("Either shell command or --pid|-p option is required\n");
            print_usage(argv[0]);
            exit(1);
        }
        shell_command_to_run = read_remaining_arguments_as_char(argc, argv);
    }
    if (quiet && debug) {
        fprintf_error("%s: Incompatible options --quiet|-q and --debug used\n", argv[0]);
        exit(1);
    }
    if (quiet && verbose) {
        fprintf_error("%s: Incompatible options --quiet|-q and --verbose|-v used\n", argv[0]);
        exit(1);
    }

}
