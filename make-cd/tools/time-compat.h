#ifndef TIME_COMPAT_H
#define TIME_COMPAT_H

#include <stdint.h>
#include <string.h>
#include <time.h>

static inline int tool_localtime_compat(time_t now, struct tm *out)
{
    const struct tm *tm_now;

    if (!out)
        return -1;

    tm_now = localtime(&now);
    if (!tm_now)
        return -1;

    *out = *tm_now;
    return 0;
}

static inline int tool_gmtime_compat(time_t now, struct tm *out)
{
    const struct tm *tm_now;

    if (!out)
        return -1;

    tm_now = gmtime(&now);
    if (!tm_now)
        return -1;

    *out = *tm_now;
    return 0;
}

static inline int tool_tm_fields_valid(const struct tm *tm_value)
{
    int year;

    if (!tm_value)
        return 0;

    year = tm_value->tm_year + 1900;
    return year >= 0 && year <= 9999 &&
           tm_value->tm_mon >= 0 && tm_value->tm_mon <= 11 &&
           tm_value->tm_mday >= 1 && tm_value->tm_mday <= 31 &&
           tm_value->tm_hour >= 0 && tm_value->tm_hour <= 23 &&
           tm_value->tm_min >= 0 && tm_value->tm_min <= 59 &&
           tm_value->tm_sec >= 0 && tm_value->tm_sec <= 60;
}

static inline void tool_write_two_digits(char *out, unsigned value)
{
    out[0] = (char)('0' + ((value / 10U) % 10U));
    out[1] = (char)('0' + (value % 10U));
}

static inline void tool_write_four_digits(char *out, unsigned value)
{
    out[0] = (char)('0' + ((value / 1000U) % 10U));
    out[1] = (char)('0' + ((value / 100U) % 10U));
    out[2] = (char)('0' + ((value / 10U) % 10U));
    out[3] = (char)('0' + (value % 10U));
}

static inline void tool_set_default_date(char out[9])
{
    memcpy(out, "19700101", 9);
}

static inline int tool_format_date_yyyymmdd(char out[9], const struct tm *tm_value)
{
    unsigned year;

    if (!out)
        return -1;

    if (!tool_tm_fields_valid(tm_value)) {
        tool_set_default_date(out);
        return -1;
    }

    year = (unsigned)(tm_value->tm_year + 1900);
    tool_write_four_digits(out, year);
    tool_write_two_digits(out + 4, (unsigned)(tm_value->tm_mon + 1));
    tool_write_two_digits(out + 6, (unsigned)tm_value->tm_mday);
    out[8] = '\0';
    return 0;
}

static inline void tool_set_default_iso9660_volume_time(uint8_t out[17])
{
    memcpy(out, "1970010100000000", 16);
    out[16] = 0;
}

static inline int tool_format_iso9660_volume_time(uint8_t out[17],
                                                  const struct tm *tm_value)
{
    unsigned year;

    if (!out)
        return -1;

    if (!tool_tm_fields_valid(tm_value)) {
        tool_set_default_iso9660_volume_time(out);
        return -1;
    }

    year = (unsigned)(tm_value->tm_year + 1900);
    tool_write_four_digits((char *)out, year);
    tool_write_two_digits((char *)out + 4, (unsigned)(tm_value->tm_mon + 1));
    tool_write_two_digits((char *)out + 6, (unsigned)tm_value->tm_mday);
    tool_write_two_digits((char *)out + 8, (unsigned)tm_value->tm_hour);
    tool_write_two_digits((char *)out + 10, (unsigned)tm_value->tm_min);
    tool_write_two_digits((char *)out + 12, (unsigned)tm_value->tm_sec);
    out[14] = '0';
    out[15] = '0';
    out[16] = 0;
    return 0;
}

#endif /* TIME_COMPAT_H */
