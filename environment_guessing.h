#ifndef RUNWHENIDLE_ENVIRONMENT_GUESSING_H
#define RUNWHENIDLE_ENVIRONMENT_GUESSING_H

void best_effort_infer_graphical_session_environment_if_missing(bool log_when_inferred);
Display *open_x11_display_best_effort(void);
struct wl_display *connect_to_wayland_best_effort(void);


#endif //RUNWHENIDLE_ENVIRONMENT_GUESSING_H
