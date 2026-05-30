/* client/playstation2/iop/smap_rtc_calendar.h
 *
 * Pure calendar / BCD / JST-bias math used by the IOP smap.irx RTC
 * driver and by host-side tests.
 *
 * Time-basis convention used here:
 *   - Calendar fields are interpreted as **proleptic Gregorian**, JST or
 *     UTC depending on context (the caller sets that — these helpers
 *     don't know).
 *   - "Unix-style seconds" means seconds since 1970-01-01 00:00:00 of
 *     whichever calendar the caller passed.  The smap_*_to/from_*
 *     helpers do not embed any timezone bias — bias is applied
 *     separately via SMAP_CDVD_RTC_JST_BIAS_SECS at the hardware
 *     boundary in smap_cdvd_read_rtc / smap_cdvd_write_rtc.
 *
 * No dependencies — header-only, plain C, 32-bit-safe.  Suitable for
 * IOP IRX builds (-G 0, no libgcc 64-bit) and for host self-tests.
 */

#ifndef SMAP_RTC_CALENDAR_H
#define SMAP_RTC_CALENDAR_H

/* PS2 mechacon stores its calendar in JST (UTC+9).  Apply this bias
 * once at the hardware boundary when converting to/from Unix UTC.
 *   read:  jst_unix - JST_BIAS = utc_unix
 *   write: utc_unix + JST_BIAS = jst_unix
 * Adjustment is done in the seconds domain so day/month/year carry
 * across midnight automatically via smap_*_to/from_*. */
#define SMAP_CDVD_RTC_JST_BIAS_SECS (9 * 60 * 60)

static int smap_bcd_to_int(unsigned char bcd) {
    unsigned int hi = (unsigned int)((bcd >> 4) & 0x0f);
    unsigned int lo = (unsigned int)(bcd & 0x0f);

    if(hi > 9 || lo > 9)
        return -1;
    return (int)(hi * 10 + lo);
}

static unsigned char smap_int_to_bcd(unsigned int value) {
    return (unsigned char)(((value / 10) << 4) | (value % 10));
}

static int smap_is_leap_year(unsigned int year) {
    if((year % 4) != 0)
        return 0;
    if((year % 100) != 0)
        return 1;
    return (year % 400) == 0;
}

static unsigned int smap_days_before_month(unsigned int year, unsigned int month) {
    static const unsigned short days[12] = {0,   31,  59,  90,  120, 151,
                                            181, 212, 243, 273, 304, 334};
    unsigned int total;

    if(month == 0 || month > 12)
        return 0;

    total = days[month - 1];
    if(month > 2 && smap_is_leap_year(year))
        total++;
    return total;
}

static unsigned int smap_days_before_year(unsigned int year) {
    unsigned int days = 0;
    unsigned int y;

    for(y = 1970; y < year; y++)
        days += smap_is_leap_year(y) ? 366 : 365;
    return days;
}

/* (year, month, day, hour, min, sec) -> Unix-style seconds.
 * Caller's calendar may be UTC or JST; the function is timezone-naive. */
static unsigned int smap_datetime_to_unix(unsigned int year, unsigned int month, unsigned int day,
                                          unsigned int hour, unsigned int minute, unsigned int second) {
    unsigned int days = smap_days_before_year(year) + smap_days_before_month(year, month) + (day - 1);

    return (((days * 24 + hour) * 60 + minute) * 60 + second);
}

/* Inverse of smap_datetime_to_unix.  Rolls year/month/day correctly. */
static void smap_unix_to_datetime(unsigned int timestamp, unsigned int *year, unsigned int *month,
                                  unsigned int *day, unsigned int *hour, unsigned int *minute,
                                  unsigned int *second) {
    unsigned int days = timestamp / 86400;
    unsigned int rem = timestamp % 86400;
    unsigned int y = 1970;
    unsigned int m;

    *hour = rem / 3600;
    rem %= 3600;
    *minute = rem / 60;
    *second = rem % 60;

    for(;;) {
        unsigned int year_days = smap_is_leap_year(y) ? 366 : 365;
        if(days < year_days)
            break;
        days -= year_days;
        y++;
    }

    for(m = 1; m < 12; m++) {
        unsigned int month_days = smap_days_before_month(y, m + 1) - smap_days_before_month(y, m);
        if(days < month_days)
            break;
        days -= month_days;
    }

    *year = y;
    *month = m;
    *day = days + 1;
}

#endif /* SMAP_RTC_CALENDAR_H */
