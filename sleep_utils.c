#include <time.h>
#include <errno.h>
#include "sleep_utils.h"

int sleep_for_milliseconds(long milliseconds)
{
    struct timespec ts;

    if (milliseconds < 0)
    {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;

    //We don't care about sleeping for exact amount in case sleep is interrupted by a signal,
    //which is why NULL is used for the second argument
    return nanosleep(&ts, NULL);
}
