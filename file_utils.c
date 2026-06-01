#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include "string_utils.h"

int file_is_socket(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISSOCK(st.st_mode) ? 1 : 0;
}

int file_is_readable_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        return 0;
    }
    return access(path, R_OK) == 0 ? 1 : 0;
}

int directory_exists_and_accessible(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return 0;
    }
    return access(path, R_OK | X_OK) == 0 ? 1 : 0;
}

int get_home_directory_for_current_user(char *out_home, size_t out_home_size) {
    const char *home_env = getenv("HOME");
    if (!is_string_null_or_empty(home_env)) {
        snprintf(out_home, out_home_size, "%s", home_env);
        return 1;
    }

    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_dir) {
        return 0;
    }

    snprintf(out_home, out_home_size, "%s", pw->pw_dir);
    return 1;
}