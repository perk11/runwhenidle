#ifndef RUNWHENIDLE_TIME_UTILS_H
#define RUNWHENIDLE_TIME_UTILS_H
#include <time.h>

/**
 * Calculates the elapsed time in milliseconds between two `struct timespec` values.
 *
 * @param start The starting time.
 * @param end The ending time.
 * @return The elapsed time in milliseconds.
 */
long long get_elapsed_time_ms(struct timespec start, struct timespec end);

#endif //RUNWHENIDLE_TIME_UTILS_H
