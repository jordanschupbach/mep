#ifndef MEP_TEX_TIME_H
#define MEP_TEX_TIME_H

// Thread-safe broken-down time and the local UTC offset, on POSIX
// (gmtime_r/localtime_r/tm_gmtoff) and Windows (gmtime_s/localtime_s and
// the CRT's timezone settings), which spell all three differently.

#include <ctime>

namespace meptex {

inline void UtcTime(std::time_t t, std::tm *out) {
#if defined(_WIN32)
    gmtime_s(out, &t);
#else
    gmtime_r(&t, out);
#endif
}

inline void LocalTime(std::time_t t, std::tm *out) {
#if defined(_WIN32)
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

/** @brief Seconds east of UTC for @p local, a LocalTime() result. */
inline long UtcOffset(const std::tm &local) {
#if defined(_WIN32)
    long tz = 0;
    _get_timezone(&tz);  // seconds west of UTC, standard time
    long off = -tz;
    if (local.tm_isdst > 0) {
        long dst = 0;
        _get_dstbias(&dst);  // negative: daylight time is that much further east
        off -= dst;
    }
    return off;
#else
    return local.tm_gmtoff;
#endif
}

}  // namespace meptex

#endif
