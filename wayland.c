#include "ext-idle-notify-v1-client-protocol.h"

#include "wayland.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/limits.h>
#include <sys/timerfd.h>

#include "arguments_parsing.h"
#include "environment_guessing.h"
#include "process_handling.h"
#include "sleep_utils.h"
#include "string_utils.h"
#include "tty_utils.h"

static struct wl_display *wayland_display = NULL;
static struct wl_registry *wayland_registry = NULL;

static struct wl_seat *wayland_seat = NULL;

static struct ext_idle_notifier_v1 *wayland_idle_notifier = NULL;

static uint32_t wayland_idle_notifier_version = 0;

static struct ext_idle_notification_v1 *wayland_idle_notification = NULL;

static int wayland_idle_notify_available = 0;


static struct wl_display *connect_to_wayland_best_effort(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (display) {
        return display;
    }

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    char inferred_runtime_dir[PATH_MAX];
    if (is_string_null_or_empty(runtime_dir)) {
        if (!build_default_xdg_runtime_dir_for_current_user(inferred_runtime_dir, sizeof(inferred_runtime_dir))) {
            return NULL;
        }
        runtime_dir = inferred_runtime_dir;
    }

    char wayland_socket_path[PATH_MAX];
    char wayland_socket_name[NAME_MAX + 1];
    if (!find_best_wayland_socket_in_runtime_dir(runtime_dir, wayland_socket_path, sizeof(wayland_socket_path),
                                                 wayland_socket_name, sizeof(wayland_socket_name))) {
        return NULL;
                                                 }

    display = wl_display_connect(wayland_socket_path);
    if (!display) {
        return NULL;
    }

    if (is_string_null_or_empty(getenv("XDG_RUNTIME_DIR"))) {
        setenv("XDG_RUNTIME_DIR", runtime_dir, 0);
    }
    if (is_string_null_or_empty(getenv("WAYLAND_DISPLAY"))) {
        setenv("WAYLAND_DISPLAY", wayland_socket_name, 0);
    }

    return display;
}

static int try_initialize_wayland_idle_backend(const struct wl_registry_listener *wayland_registry_listener) {
    wayland_display = connect_to_wayland_best_effort();
    if (!wayland_display) {
        return 0;
    }

    wayland_registry = wl_display_get_registry(wayland_display);
    if (!wayland_registry) {
        wl_display_disconnect(wayland_display);
        wayland_display = NULL;
        return 0;
    }

    wl_registry_add_listener(wayland_registry, wayland_registry_listener, NULL);
    wl_display_roundtrip(wayland_display);

    if (wayland_seat == NULL || wayland_idle_notifier == NULL) {
        wl_registry_destroy(wayland_registry);
        wayland_registry = NULL;
        wl_display_disconnect(wayland_display);
        wayland_display = NULL;
        wayland_seat = NULL;
        wayland_idle_notifier = NULL;
        wayland_idle_notify_available = 0;
        return 0;
    }

    wayland_idle_notify_available = 1;
    return 1;
}

int start_wayland_idle_notification_object(const struct ext_idle_notification_v1_listener *wayland_idle_notification_listener) {
    if (!wayland_idle_notify_available || wayland_idle_notification != NULL) {
        return 0;
    }

    uint32_t timeout_ms_for_protocol = (user_idle_timeout_ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)user_idle_timeout_ms;

    if (wayland_idle_notifier_version >= 2) {
        wayland_idle_notification = ext_idle_notifier_v1_get_input_idle_notification(
                wayland_idle_notifier, timeout_ms_for_protocol, wayland_seat);
    } else {
        wayland_idle_notification = ext_idle_notifier_v1_get_idle_notification(
                wayland_idle_notifier, timeout_ms_for_protocol, wayland_seat);
    }

    if (!wayland_idle_notification) {
        return -1;
    }

    ext_idle_notification_v1_add_listener(wayland_idle_notification, wayland_idle_notification_listener, NULL);
    wl_display_flush(wayland_display);
    return 1;
}
static void wayland_registry_global(void *data,
                                    struct wl_registry *registry,
                                    uint32_t name,
                                    const char *interface,
                                    uint32_t version) {
    (void)data;

    if (strcmp(interface, "wl_seat") == 0 && wayland_seat == NULL) {
        wayland_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        return;
    }

    if (strcmp(interface, "ext_idle_notifier_v1") == 0 && wayland_idle_notifier == NULL) {
        uint32_t bind_version = version < 2 ? version : 2;
        wayland_idle_notifier_version = bind_version;
        wayland_idle_notifier = wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, bind_version);
    }
}

static void wayland_registry_global_remove(void *data,
                                           struct wl_registry *registry,
                                           uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener wayland_registry_listener = {
    .global = wayland_registry_global,
    .global_remove = wayland_registry_global_remove
};

int try_monitor_wayland_idle_notify(WaylandLoopFunction wayland_loop_function) {
    if (!try_initialize_wayland_idle_backend(&wayland_registry_listener)) {
        return -1;
    }
    int wayland_loop_result = wayland_loop_function(wayland_display);

    if (wayland_idle_notification) {
        ext_idle_notification_v1_destroy(wayland_idle_notification);
        wayland_idle_notification = NULL;
    }
    if (wayland_idle_notifier) {
        ext_idle_notifier_v1_destroy(wayland_idle_notifier);
        wayland_idle_notifier = NULL;
    }
    if (wayland_seat) {
        wl_seat_destroy(wayland_seat);
        wayland_seat = NULL;
    }
    if (wayland_registry) {
        wl_registry_destroy(wayland_registry);
        wayland_registry = NULL;
    }
    if (wayland_display) {
        wl_display_disconnect(wayland_display);
        wayland_display = NULL;
    }

    return wayland_loop_result;
}


