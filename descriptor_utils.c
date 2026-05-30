#include "descriptor_utils.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <asm-generic/errno-base.h>
#include <sys/timerfd.h>

#include "tty_utils.h"

int create_one_shot_timer_file_descriptor_after_ms(long delay_ms) {
    int timer_file_descriptor = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_file_descriptor < 0) {
        return -1;
    }

    struct itimerspec timer_spec = {0};
    timer_spec.it_value.tv_sec = delay_ms / 1000;
    timer_spec.it_value.tv_nsec = (delay_ms % 1000) * 1000000L;

    if (timerfd_settime(timer_file_descriptor, 0, &timer_spec, NULL) < 0) {
        close(timer_file_descriptor);
        return -1;
    }

    return timer_file_descriptor;
}
int create_periodic_timer_file_descriptor_every_ms(long interval_ms) {
    int timer_file_descriptor = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_file_descriptor < 0) {
        return -1;
    }

    struct itimerspec timer_spec = {0};
    timer_spec.it_value.tv_sec = interval_ms / 1000;
    timer_spec.it_value.tv_nsec = (interval_ms % 1000) * 1000000L;
    timer_spec.it_interval = timer_spec.it_value;

    if (timerfd_settime(timer_file_descriptor, 0, &timer_spec, NULL) < 0) {
        close(timer_file_descriptor);
        return -1;
    }

    return timer_file_descriptor;
}

void close_file_descriptor_if_open(int *file_descriptor, const char *description) {
    if (*file_descriptor < 0) {
        return;
    }

    if (close(*file_descriptor) < 0) {
        const int saved_errno = errno;
        fprintf_error("Failed to close %s file descriptor %d: %s\n",
                      description,
                      *file_descriptor,
                      strerror(saved_errno));
    }

    *file_descriptor = -1;
}

int consume_timer_file_descriptor_checked(int timer_file_descriptor, const char *description) {
    uint64_t expirations = 0;
    ssize_t bytes_read;

    do {
        bytes_read = read(timer_file_descriptor, &expirations, sizeof(expirations));
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read == sizeof(expirations)) {
        return 0;
    }

    if (bytes_read < 0 && errno == EAGAIN) {
        return 0;
    }

    if (bytes_read < 0) {
        const int saved_errno = errno;
        fprintf_error("Failed to read %s timer file descriptor: %s\n",
                      description,
                      strerror(saved_errno));
    } else {
        fprintf_error("Short read from %s timer file descriptor: got %zd bytes, expected %zu\n",
                      description,
                      bytes_read,
                      sizeof(expirations));
    }

    return -1;
}
