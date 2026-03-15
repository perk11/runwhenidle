#include <dirent.h>
#include <limits.h>
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
    DIR *dir = opendir(runtime_dir);
    if (!dir) {
        return 0;
    }

    int found = 0;
    int best_numeric_suffix = INT_MAX;
    char best_name[NAME_MAX + 1];
    best_name[0] = '\0';

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "wayland-", 8) != 0) {
            continue;
        }

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", runtime_dir, entry->d_name);

        if (!file_is_socket(full_path)) {
            continue;
        }

        int numeric_suffix = -1;
        const char *suffix = entry->d_name + 8;
        if (*suffix >= '0' && *suffix <= '9') {
            numeric_suffix = (int)strtol(suffix, NULL, 10);
        }

        if (!found) {
            found = 1;
            best_numeric_suffix = (numeric_suffix >= 0) ? numeric_suffix : INT_MAX;
            snprintf(best_name, sizeof(best_name), "%s", entry->d_name);
            continue;
        }

        if (numeric_suffix >= 0 && numeric_suffix < best_numeric_suffix) {
            best_numeric_suffix = numeric_suffix;
            snprintf(best_name, sizeof(best_name), "%s", entry->d_name);
            continue;
        }

        if (best_numeric_suffix == INT_MAX && numeric_suffix == -1) {
            snprintf(best_name, sizeof(best_name), "%s", entry->d_name);
        }
    }

    closedir(dir);

    if (!found) {
        return 0;
    }

    snprintf(out_socket_path, out_socket_path_size, "%s/%s", runtime_dir, best_name);
    if (out_socket_name && out_socket_name_size > 0) {
        snprintf(out_socket_name, out_socket_name_size, "%s", best_name);
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
    size_t home_dir_length = strnlen(home_dir, xauthority_path_size);
    size_t suffix_length = sizeof(xauthority_suffix) - 1;

    if (home_dir_length == 0 || home_dir_length >= xauthority_path_size) {
        return 0;
    }

    if (home_dir_length + suffix_length + 1 > xauthority_path_size) {
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

        int display_number = (int)strtol(digits, NULL, 10);

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

void best_effort_infer_graphical_session_environment_if_missing(bool log_when_inferred) {
     if (!is_string_null_or_empty(getenv("WAYLAND_DISPLAY")) || !is_string_null_or_empty(getenv("DISPLAY"))) {
         return;
     }

     char inferred_runtime_dir[PATH_MAX];
     if (is_string_null_or_empty(getenv("XDG_RUNTIME_DIR"))) {
         if (build_default_xdg_runtime_dir_for_current_user(inferred_runtime_dir, sizeof(inferred_runtime_dir))) {
             setenv("XDG_RUNTIME_DIR", inferred_runtime_dir, 0);
         }
     }

     const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
     if (!is_string_null_or_empty(runtime_dir) && is_string_null_or_empty(getenv("WAYLAND_DISPLAY"))) {
         char wayland_socket_path[PATH_MAX];
         char wayland_socket_name[NAME_MAX + 1];
         if (find_best_wayland_socket_in_runtime_dir(runtime_dir, wayland_socket_path, sizeof(wayland_socket_path),
                                                     wayland_socket_name, sizeof(wayland_socket_name))) {
             setenv("WAYLAND_DISPLAY", wayland_socket_name, 0);
                                                     }
     }

     if (is_string_null_or_empty(getenv("DISPLAY"))) {
         char inferred_display[32];
         if (find_best_x11_display_from_socket_dir(inferred_display, sizeof(inferred_display))) {
             setenv("DISPLAY", inferred_display, 0);
             ensure_xauthority_is_set_if_possible();
         }
     }

     if (log_when_inferred) {
         const char *wd = getenv("WAYLAND_DISPLAY");
         const char *xdg = getenv("XDG_RUNTIME_DIR");
         const char *dpy = getenv("DISPLAY");
         fprintf(stderr, "inferred session vars: XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s DISPLAY=%s\n",
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