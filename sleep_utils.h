#ifndef SLEEP_UTILS_H
#define SLEEP_UTILS_H

#include <time.h>
#include <errno.h>

/**
 * Sleeps for the specified number of milliseconds unless interrupted by a signal.
 *
 * @param milliseconds The number of milliseconds to sleep.
 * @return 0 on success, -1 on failure or being interrupted by a signal (sets errno to EINVAL if milliseconds is negative).
 */
int sleep_for_milliseconds(long milliseconds);

#endif /* SLEEP_UTILS_H */
