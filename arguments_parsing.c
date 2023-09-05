#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "output_settings.h"
#include "arguments_parsing.h"
#include "tty_utils.h"

const long TIMEOUT_MAX_SUPPORTED_VALUE = 100000000; //~3 years
const long TIMEOUT_MIN_SUPPORTED_VALUE = 1;
const long START_MONITOR_AFTER_MAX_SUPPORTED_VALUE = TIMEOUT_MAX_SUPPORTED_VALUE*1000;
const long START_MONITOR_AFTER_MIN_SUPPORTED_VALUE = 0;

void print_usage(char *binary_name) {
    printf("Usage: %s [OPTIONS] shell_command_to_run [shell_command_arguments]\n", binary_name);
    printf("\nOptions:\n");
    printf("  --timeout, -t <timeout_value_in_seconds>  Set the user idle time after which the command can run in seconds (default: 300 seconds).\n");
    printf("  --start-monitor-after, -a <delay_in_ms>   Set an initial delay in milliseconds before monitoring starts. During this time command runs unrestricted. This helps to catch quick errors. (default: 300 ms).\n");
    printf("  --verbose, -v                             Enable verbose output for monitoring.\n");
    printf("  --debug                                   Enable debugging output.\n");
    printf("  --quiet, -q                               Suppress all program output except errors.\n");
    printf("  --version, -V                             Print the program version information.\n");
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
    remaining_arguments_string[current_length_of_all_arguments - 1] = '\0'; // Replace the last space separator with a null terminator

    return remaining_arguments_string;
}

char *parse_command_line_arguments(int argc, char *argv[]) {
    struct option long_options[] = {
            {"timeout", required_argument, NULL, 't'},
            {"start-monitor-after", required_argument, NULL, 'a'},
            {"pause-method", required_argument, NULL, 'm'},
            {"verbose", no_argument,       NULL, 'v'},
            {"debug", no_argument,         NULL, 'd'},
            {"quiet",   no_argument,       NULL, 'q'},
            {"help",    no_argument,       NULL, 'h'},
            {"version",    no_argument,    NULL, 'V'},
            {NULL, 0,                      NULL, 0}
    };

    // Parse command line options
    int option;
    while ((option = getopt_long(argc, argv, "+hvqt:a:V", long_options, NULL)) != -1) {
        switch (option) {
            case 't': {

                long timeout_arg_value = strtol(optarg, NULL, 10);
                if (timeout_arg_value < TIMEOUT_MIN_SUPPORTED_VALUE ||
                    timeout_arg_value > TIMEOUT_MAX_SUPPORTED_VALUE || errno != 0) {
                    fprintf_error("Invalid timeout value: \"%s\". Range supported: %ld-%ld", optarg,
                                  TIMEOUT_MIN_SUPPORTED_VALUE, TIMEOUT_MAX_SUPPORTED_VALUE);
                    print_usage(argv[0]);
                    exit(1);
                }
                user_idle_timeout_ms = timeout_arg_value * 1000;
                break;
            }
            case 'a': {
                start_monitor_after_ms = strtol(optarg, NULL, 10);

                if (start_monitor_after_ms < START_MONITOR_AFTER_MIN_SUPPORTED_VALUE || errno != 0) {
                    fprintf_error( "Invalid start-monitor-after time value: \"%s\" Range supported: %ld-%ld.\n", optarg,
                                   START_MONITOR_AFTER_MIN_SUPPORTED_VALUE, START_MONITOR_AFTER_MAX_SUPPORTED_VALUE
                    );
                    print_usage(argv[0]);
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
                print_usage(argv[0]);
                exit(1);
        }
    }
    if (debug) fprintf(stderr, "verbose: %i, debug: %i, quiet: %i, user_idle_timeout_ms: %i, start_monitoring_after_ms: %lld\n", verbose, debug, quiet, user_idle_timeout_ms, start_monitor_after_ms);
    if (optind >= argc) {
        print_usage(argv[0]);
        exit(1);
    }
    if (quiet && debug) {
        fprintf_error("Incompatible options --quiet|-q and --debug used");
        print_usage(argv[0]);
        exit(1);
    }
    if (quiet && verbose) {
        fprintf_error("Incompatible options --quiet|-q and --verbose|-v used");
        print_usage(argv[0]);
        exit(1);
    }

    return read_remaining_arguments_as_char(argc, argv);
}
