#ifndef RUNWHENIDLE_ENVIRONMENT_GUESSING_H
#define RUNWHENIDLE_ENVIRONMENT_GUESSING_H
#include <X11/Xlib.h>

void best_effort_infer_graphical_session_environment_if_missing(bool log_when_inferred);
Display *open_x11_display_best_effort(void);
int build_default_xdg_runtime_dir_for_current_user(char *out_runtime_dir, size_t out_runtime_dir_size);
int find_best_wayland_socket_in_runtime_dir(const char *runtime_dir,
                                                   char *out_socket_path,
                                                   size_t out_socket_path_size,
                                                   char *out_socket_name,
                                                   size_t out_socket_name_size);
#endif //RUNWHENIDLE_ENVIRONMENT_GUESSING_H
