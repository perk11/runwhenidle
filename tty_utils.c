#include <stdio.h>
#include <unistd.h>
#include "tty_utils.h"

void print_colored_text(FILE *stream, const char *color, const char *message) {
    if (isatty(fileno(stream))) {
        fprintf(stream, "\033[%sm%s\033[0m\n", color, message);
    } else {
        fprintf(stream, "%s\n", message);
    }
}

void print_error(const char *message) {
    print_colored_text(stderr, "31", message);
}