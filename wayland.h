#ifndef RUNWHENIDLE_WAYLAND_H
#define RUNWHENIDLE_WAYLAND_H
typedef int (*WaylandLoopFunction)(struct wl_display*);
int try_monitor_wayland_idle_notify(WaylandLoopFunction wayland_loop_function);
int start_wayland_idle_notification_object(const struct ext_idle_notification_v1_listener *wayland_idle_notification_listener);

#endif //RUNWHENIDLE_WAYLAND_H
