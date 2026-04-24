#ifndef TIME_COMPAT_H
#define TIME_COMPAT_H

#include <time.h>

static inline int tool_localtime_compat(time_t now, struct tm *out)
{
    if (!out)
        return -1;

#if defined(_WIN32)
    return localtime_s(out, &now) == 0 ? 0 : -1;
#else
    return localtime_r(&now, out) ? 0 : -1;
#endif
}

static inline int tool_gmtime_compat(time_t now, struct tm *out)
{
    if (!out)
        return -1;

#if defined(_WIN32)
    return gmtime_s(out, &now) == 0 ? 0 : -1;
#else
    return gmtime_r(&now, out) ? 0 : -1;
#endif
}

#endif /* TIME_COMPAT_H */
