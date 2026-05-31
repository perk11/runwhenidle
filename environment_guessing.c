#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include <X11/Xlib.h>
#include "file_utils.h"
#include "string_utils.h"

int find_best_wayland_socket_in_runtime_dir(const char *runtime_dir,
                                            char *out_socket_path,
                                            size_t out_socket_path_size,
                                            char *out_socket_name,
                                            size_t out_socket_name_size) {
    if (out_socket_path_size > 0) {
        out_socket_path[0] = '\0';
    }
    if (out_socket_name && out_socket_name_size > 0) {
        out_socket_name[0] = '\0';
    }

    DIR *directory_stream = opendir(runtime_dir);
    if (!directory_stream) {
        return 0;
    }

    int has_found_valid_wayland_socket = 0;
    int lowest_numeric_socket_suffix_found = INT_MAX;
    char best_wayland_socket_name_found[NAME_MAX + 1];
    best_wayland_socket_name_found[0] = '\0';

    struct dirent *directory_entry;
    while ((directory_entry = readdir(directory_stream)) != NULL) {
        if (strncmp(directory_entry->d_name, "wayland-", 8) != 0) {
            continue;
        }

        char absolute_socket_path_buffer[PATH_MAX];
        int required_absolute_path_length = snprintf(absolute_socket_path_buffer,
                                                     sizeof(absolute_socket_path_buffer),
                                                     "%s/%s",
                                                     runtime_dir,
                                                     directory_entry->d_name);

        if (required_absolute_path_length < 0 || (size_t)required_absolute_path_length >= sizeof(absolute_socket_path_buffer)) {
            continue;
        }

        if (!file_is_socket(absolute_socket_path_buffer)) {
            continue;
        }

        int current_socket_numeric_suffix_value = -1;
        const char *socket_name_suffix_pointer = directory_entry->d_name + 8;

        if (*socket_name_suffix_pointer >= '0' && *socket_name_suffix_pointer <= '9') {
            char *suffix_parsing_end_pointer;
            errno = 0;
            long parsed_numeric_suffix_value = strtol(socket_name_suffix_pointer, &suffix_parsing_end_pointer, 10);

            if (*suffix_parsing_end_pointer == '\0' &&
                errno != ERANGE &&
                parsed_numeric_suffix_value >= 0 &&
                parsed_numeric_suffix_value <= INT_MAX) {
                current_socket_numeric_suffix_value = (int)parsed_numeric_suffix_value;
            }
        }

        if (!has_found_valid_wayland_socket) {
            has_found_valid_wayland_socket = 1;
            lowest_numeric_socket_suffix_found = (current_socket_numeric_suffix_value >= 0) ? current_socket_numeric_suffix_value : INT_MAX;
            snprintf(best_wayland_socket_name_found, sizeof(best_wayland_socket_name_found), "%s", directory_entry->d_name);
        } else if (current_socket_numeric_suffix_value >= 0) {
            if (lowest_numeric_socket_suffix_found == INT_MAX || current_socket_numeric_suffix_value < lowest_numeric_socket_suffix_found) {
                lowest_numeric_socket_suffix_found = current_socket_numeric_suffix_value;
                snprintf(best_wayland_socket_name_found, sizeof(best_wayland_socket_name_found), "%s", directory_entry->d_name);
            }
        } else if (lowest_numeric_socket_suffix_found == INT_MAX) {
            if (strcmp(directory_entry->d_name, best_wayland_socket_name_found) < 0) {
                snprintf(best_wayland_socket_name_found, sizeof(best_wayland_socket_name_found), "%s", directory_entry->d_name);
            }
        }
    }

    closedir(directory_stream);

    if (!has_found_valid_wayland_socket) {
        return 0;
    }

    int required_final_socket_path_length = snprintf(out_socket_path,
                                                     out_socket_path_size,
                                                     "%s/%s",
                                                     runtime_dir,
                                                     best_wayland_socket_name_found);

    if (required_final_socket_path_length < 0 || (size_t)required_final_socket_path_length >= out_socket_path_size) {
        out_socket_path[0] = '\0';
        return 0;
    }

    if (out_socket_name && out_socket_name_size > 0) {
        int required_final_socket_name_length = snprintf(out_socket_name,
                                                         out_socket_name_size,
                                                         "%s",
                                                         best_wayland_socket_name_found);

        if (required_final_socket_name_length < 0 || (size_t)required_final_socket_name_length >= out_socket_name_size) {
            out_socket_path[0] = '\0';
            out_socket_name[0] = '\0';
            return 0;
        }
    }

    return 1;
}

int build_default_xdg_runtime_dir_for_current_user(char *out_runtime_dir, size_t out_runtime_dir_size) {
    uid_t uid = getuid();
    snprintf(out_runtime_dir, out_runtime_dir_size, "/run/user/%u", (unsigned)uid);
    if (!directory_exists_and_accessible(out_runtime_dir)) {
        return 0;
    }
    return 1;
}
static int build_xauthority_path_from_home_dir(char *xauthority_path,
                                               size_t xauthority_path_size,
                                               const char *home_dir) {
    static const char xauthority_suffix[] = "/.Xauthority";
    size_t home_dir_length = strlen(home_dir);
    size_t suffix_length = sizeof(xauthority_suffix) - 1;

    if (home_dir_length == 0 || home_dir_length > xauthority_path_size - suffix_length - 1) {
        return 0;
    }

    memcpy(xauthority_path, home_dir, home_dir_length);
    memcpy(xauthority_path + home_dir_length, xauthority_suffix, suffix_length + 1);
    return 1;
}
static void ensure_xauthority_is_set_if_possible(void) {
    if (!is_string_null_or_empty(getenv("XAUTHORITY"))) {
        return;
    }

    char home_dir[PATH_MAX];
    if (!get_home_directory_for_current_user(home_dir, sizeof(home_dir))) {
        return;
    }

    char xauthority_path[PATH_MAX];
    if (!build_xauthority_path_from_home_dir(xauthority_path, sizeof(xauthority_path), home_dir)) {
        return;
    }

    if (file_is_readable_regular_file(xauthority_path)) {
        setenv("XAUTHORITY", xauthority_path, 0);
    }
}
static int find_best_x11_display_from_socket_dir(char *out_display, size_t out_display_size) {
    const char *x11_socket_dir = "/tmp/.X11-unix";
    DIR *dir = opendir(x11_socket_dir);
    if (!dir) {
        return 0;
    }

    int found = 0;
    int best_display_number = INT_MAX;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != 'X') {
            continue;
        }
        const char *digits = entry->d_name + 1;
        if (*digits < '0' || *digits > '9') {
            continue;
        }

        char *endptr;
        long parsed = strtol(digits, &endptr, 10);
        if (*endptr != '\0') {
            continue;
        }
        int display_number = (int)parsed;

        char socket_path[PATH_MAX];
        snprintf(socket_path, sizeof(socket_path), "%s/%s", x11_socket_dir, entry->d_name);

        if (!file_is_socket(socket_path)) {
            continue;
        }

        if (!found || display_number < best_display_number) {
            found = 1;
            best_display_number = display_number;
        }
    }

    closedir(dir);

    if (!found) {
        return 0;
    }

    snprintf(out_display, out_display_size, ":%d", best_display_number);
    return 1;
}

void best_effort_infer_graphical_session_environment_if_missing(const bool verbose) {
     if (!is_string_null_or_empty(getenv("WAYLAND_DISPLAY")) || !is_string_null_or_empty(getenv("DISPLAY"))) {
         return;
     }

     char inferred_runtime_dir[PATH_MAX];
     if (is_string_null_or_empty(getenv("XDG_RUNTIME_DIR"))) {
         if (build_default_xdg_runtime_dir_for_current_user(inferred_runtime_dir, sizeof(inferred_runtime_dir))) {
             setenv("XDG_RUNTIME_DIR", inferred_runtime_dir, 0);
             if (verbose) {
                 fprintf(stderr, "XDG_RUNTIME_DIR was missing in env and got inferred to \"%s\"\n", inferred_runtime_dir);
             }
         }
     }

     const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
     if (!is_string_null_or_empty(runtime_dir) && is_string_null_or_empty(getenv("WAYLAND_DISPLAY"))) {
         char wayland_socket_path[PATH_MAX];
         char wayland_socket_name[NAME_MAX + 1];
         if (find_best_wayland_socket_in_runtime_dir(runtime_dir, wayland_socket_path, sizeof(wayland_socket_path),
                                                     wayland_socket_name, sizeof(wayland_socket_name))) {
             setenv("WAYLAND_DISPLAY", wayland_socket_name, 0);
             if (verbose) {
                 fprintf(stderr, "WAYLAND_DISPLAY was missing in env and got inferred to \"%s\"\n", wayland_socket_name);
             }
         }
     }

     if (is_string_null_or_empty(getenv("DISPLAY"))) {
         char inferred_display[32];
         if (find_best_x11_display_from_socket_dir(inferred_display, sizeof(inferred_display))) {
             setenv("DISPLAY", inferred_display, 0);
             if (verbose) {
                 fprintf(stderr, "DISPLAY was missing in env and got inferred to \"%s\"\n", inferred_display);
             }
             ensure_xauthority_is_set_if_possible();
         }
     }

     if (verbose) {
         const char *wd = getenv("WAYLAND_DISPLAY");
         const char *xdg = getenv("XDG_RUNTIME_DIR");
         const char *dpy = getenv("DISPLAY");
         fprintf(stderr, "session vars: XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s DISPLAY=%s\n",
                 xdg ? xdg : "(unset)", wd ? wd : "(unset)", dpy ? dpy : "(unset)");
     }
 }


Display *open_x11_display_best_effort(void) {
     Display *display = XOpenDisplay(NULL);
     if (display) {
         return display;
     }

     if (!is_string_null_or_empty(getenv("DISPLAY"))) {
         return NULL;
     }

     char inferred_display[32];
     if (!find_best_x11_display_from_socket_dir(inferred_display, sizeof(inferred_display))) {
         return NULL;
     }

     setenv("DISPLAY", inferred_display, 0);
     ensure_xauthority_is_set_if_possible();
     return XOpenDisplay(NULL);
 }