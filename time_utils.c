#include "time_utils.h"
long long get_elapsed_time_ms(struct timespec start, struct timespec end) {
    long long start_ms = start.tv_sec * 1000LL + start.tv_nsec / 1000000LL;
    long long end_ms = end.tv_sec * 1000LL + end.tv_nsec / 1000000LL;
    return end_ms - start_ms;
}
