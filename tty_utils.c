#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdbool.h>
#include "tty_utils.h"

void print_colored_prefix(FILE *stream, const char *color, bool is_tty) {
    if (is_tty) {
        fprintf(stream, "\033[%sm", color);
    }
}

void print_colored_suffix(FILE *stream, bool is_tty) {
    if (is_tty) {
        fprintf(stream, "\033[0m\n");
    } else {
        fprintf(stream, "\n");
    }
}

void fprintf_error(const char *format, ...) {
    va_list args;
    va_start(args, format);

    bool is_tty = isatty(fileno(stderr));
    print_colored_prefix(stderr, "31", is_tty);
    vfprintf(stderr, format, args);
    print_colored_suffix(stderr, is_tty);

    va_end(args);
}