#include "string_utils.h"

#include <stddef.h>

int is_string_null_or_empty(const char *value) {
    return value == NULL || value[0] == '\0';
}
