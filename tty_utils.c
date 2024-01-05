#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdbool.h>
#include "tty_utils.h"

void print_colored_prefix(FILE *stream, const char *color) {
    fprintf(stream, "\033[%sm", color);
}

void print_colored_suffix(FILE *stream) {
    fprintf(stream, "\033[0m");
}

void fprintf_error(const char *format, ...) {
    va_list args;
    va_start(args, format);

    bool is_tty = isatty(fileno(stderr));
    if (is_tty) print_colored_prefix(stderr, "31");
    vfprintf(stderr, format, args);
    if (is_tty) print_colored_suffix(stderr);

    va_end(args);
}