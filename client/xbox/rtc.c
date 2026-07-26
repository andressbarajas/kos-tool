#include <stdint.h>
#include <stdbool.h>

#define RTC_INDEX_PORT 0x70u
#define RTC_DATA_PORT  0x71u
#define TSC_PER_US     733u
#define UIP_LIMIT_US   10000u

enum xbox_clean_rtc_status {
    XBOX_CLEAN_RTC_OK = 0,
    XBOX_CLEAN_RTC_NO_SHADOW,
    XBOX_CLEAN_RTC_UIP_TIMEOUT,
    XBOX_CLEAN_RTC_ALREADY_FROZEN,
    XBOX_CLEAN_RTC_NOT_UTC
};

volatile uint32_t xbox_clean_rtc_last_status = XBOX_CLEAN_RTC_NO_SHADOW;
volatile uint8_t xbox_clean_rtc_index_shadow;
volatile bool xbox_clean_rtc_index_shadow_valid;
static volatile uint32_t rtc_lock;

struct rtc_fields {
    uint16_t year;
    uint8_t month, day, weekday, hour, minute, second;
};

static inline void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint64_t rdtsc_value(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t irq_save_disable(void) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli"
                     : "=r"(flags) : : "memory", "cc");
    return flags;
}

static inline void irq_restore(uint32_t flags) {
    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

static void lock_acquire(void) {
    while(__sync_lock_test_and_set(&rtc_lock, 1u) != 0u)
        __asm__ volatile("pause" : : : "memory");
}

static void lock_release(void) {
    __sync_lock_release(&rtc_lock);
}

static bool leap_year(uint32_t year) {
    return (year % 4u) == 0u &&
           ((year % 100u) != 0u || (year % 400u) == 0u);
}

static void convert_timestamp(uint32_t timestamp, struct rtc_fields *out) {
    static const uint8_t month_days[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint32_t days = timestamp / 86400u;
    uint32_t day_remainder = timestamp % 86400u;
    uint32_t remaining_days = days;
    uint32_t year = 1970u;
    uint32_t month = 0;

    out->hour = (uint8_t)(day_remainder / 3600u);
    day_remainder %= 3600u;
    out->minute = (uint8_t)(day_remainder / 60u);
    out->second = (uint8_t)(day_remainder % 60u);
    out->weekday = (uint8_t)(((days + 4u) % 7u) + 1u);

    for(;;) {
        uint32_t length = leap_year(year) ? 366u : 365u;
        if(remaining_days < length)
            break;
        remaining_days -= length;
        ++year;
    }
    while(month < 12u) {
        uint32_t length = month_days[month];
        if(month == 1u && leap_year(year))
            ++length;
        if(remaining_days < length)
            break;
        remaining_days -= length;
        ++month;
    }

    out->year = (uint16_t)year;
    out->month = (uint8_t)(month + 1u);
    out->day = (uint8_t)(remaining_days + 1u);
}

static void rtc_select(uint8_t reg) {
    xbox_clean_rtc_index_shadow = (uint8_t)(0x80u | (reg & 0x7fu));
    out8(RTC_INDEX_PORT, xbox_clean_rtc_index_shadow);
}

static uint8_t rtc_read(uint8_t reg) {
    rtc_select(reg);
    return in8(RTC_DATA_PORT);
}

static void rtc_write(uint8_t reg, uint8_t value) {
    rtc_select(reg);
    out8(RTC_DATA_PORT, value);
}

static uint8_t encode_value(uint32_t value, bool binary) {
    if(binary)
        return (uint8_t)value;
    return (uint8_t)(((value / 10u) << 4) | (value % 10u));
}

void xbox_clean_rtc_init(void) {
    uint32_t flags = irq_save_disable();

    lock_acquire();
    out8(RTC_INDEX_PORT, 0x0au);
    xbox_clean_rtc_index_shadow = 0x0au;
    xbox_clean_rtc_index_shadow_valid = true;
    lock_release();
    irq_restore(flags);
}

void xbox_clean_set_rtc(uint32_t unix_timestamp) {
    struct rtc_fields value;
    uint32_t flags;
    uint8_t saved_index;
    uint8_t saved_b;
    bool binary;
    bool hour24;
    uint8_t encoded_hour;
    uint64_t started;
    uint32_t polls = 2560000u;

    convert_timestamp(unix_timestamp, &value);
    flags = irq_save_disable();
    lock_acquire();

    if(!xbox_clean_rtc_index_shadow_valid) {
        xbox_clean_rtc_last_status = XBOX_CLEAN_RTC_NO_SHADOW;
        goto out_unlock;
    }
    saved_index = xbox_clean_rtc_index_shadow;

    started = rdtsc_value();
    while((rtc_read(0x0au) & 0x80u) != 0u) {
        if(rdtsc_value() - started >=
               (uint64_t)UIP_LIMIT_US * TSC_PER_US ||
           --polls == 0u) {
            xbox_clean_rtc_last_status = XBOX_CLEAN_RTC_UIP_TIMEOUT;
            goto out_restore_index;
        }
        __asm__ volatile("pause" : : : "memory");
    }

    saved_b = rtc_read(0x0bu);
    if((saved_b & 0x80u) != 0u) {
        xbox_clean_rtc_last_status = XBOX_CLEAN_RTC_ALREADY_FROZEN;
        goto out_restore_index;
    }
    if((saved_b & 0x01u) != 0u) {
        xbox_clean_rtc_last_status = XBOX_CLEAN_RTC_NOT_UTC;
        goto out_restore_index;
    }

    binary = (saved_b & 0x04u) != 0u;
    hour24 = (saved_b & 0x02u) != 0u;
    if(hour24) {
        encoded_hour = encode_value(value.hour, binary);
    }
    else {
        uint8_t hour12 = (uint8_t)(value.hour % 12u);
        bool pm = value.hour >= 12u;
        if(hour12 == 0u)
            hour12 = 12u;
        encoded_hour = encode_value(hour12, binary);
        if(pm)
            encoded_hour |= 0x80u;
    }

    rtc_write(0x0bu, (uint8_t)(saved_b | 0x80u));
    rtc_write(0x00u, encode_value(value.second, binary));
    rtc_write(0x02u, encode_value(value.minute, binary));
    rtc_write(0x04u, encoded_hour);
    rtc_write(0x06u, encode_value(value.weekday, binary));
    rtc_write(0x07u, encode_value(value.day, binary));
    rtc_write(0x08u, encode_value(value.month, binary));
    rtc_write(0x09u, encode_value(value.year % 100u, binary));
    rtc_write(0x7fu, encode_value(value.year / 100u, binary));
    rtc_write(0x0bu, saved_b);
    xbox_clean_rtc_last_status = XBOX_CLEAN_RTC_OK;

out_restore_index:
    xbox_clean_rtc_index_shadow = saved_index;
    out8(RTC_INDEX_PORT, saved_index);
out_unlock:
    lock_release();
    irq_restore(flags);
}
