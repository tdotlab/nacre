/* The real-time clock, a Seiko S-35180 on a bit-banged three-wire bus at
 * 0x04000138 on the ARM7 (GBATEK, "DS Real-Time Clock (RTC)").
 *
 * The game drives chip select, clock and data. Bits travel LSB first, written
 * on the falling clock edge and sampled on the rising one. A command byte picks
 * a register and a direction, then its parameter bytes follow. Commands sent
 * MSB first, as Seiko's own datasheet suggests, are understood too.
 *
 * The clock counts seconds from 2000-01-01 00:00; the DS keeps local time, and
 * the year register only reaches 2099, so the count wraps every 100 years.
 * GBATEK calls the day-of-week numbering a custom assignment; we count from
 * 0 = Sunday. */

#include "nds_internal.h"

#include <string.h>

#define SECONDS_PER_DAY 86400
#define CENTURY_DAYS 36525 /* 2000-2099, 2000 being a leap year */

enum { CMD_STAT1 = 0, CMD_INT1 = 1, CMD_DATETIME = 2, CMD_ADJUST = 3, CMD_STAT2 = 4,
       CMD_ALARM2 = 5, CMD_TIME = 6, CMD_FREE = 7 };

static uint8_t bcd(unsigned v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }
static unsigned from_bcd(uint8_t v) { return (v >> 4) * 10u + (v & 15u); }

static int leap(unsigned year) { return year % 4 == 0; } /* right for 2000-2099 */

static unsigned month_days(unsigned year, unsigned month) {
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return days[month - 1] + (month == 2 && leap(year));
}

static int64_t now_seconds(const Nds *nds) {
    const NdsRtc *r = &nds->rtc;
    int64_t t = r->base + r->offset + (int64_t)(nds_now(nds, 1) / NDS_ARM7_HZ);
    int64_t century = (int64_t)CENTURY_DAYS * SECONDS_PER_DAY;
    t %= century;
    return t < 0 ? t + century : t;
}

/* Sets the clock to `t` seconds since 2000, by moving the offset. */
static void set_seconds(Nds *nds, int64_t t) {
    NdsRtc *r = &nds->rtc;
    r->offset += t - now_seconds(nds);
}

static uint8_t hour_reg(const Nds *nds, unsigned hour) {
    uint8_t pm = hour >= 12 ? 0x40 : 0;
    if (!(nds->rtc.stat1 & 2)) hour %= 12; /* 12-hour mode: noon is 00h with PM */
    return (uint8_t)(bcd(hour) | pm);
}

static unsigned hour_value(const Nds *nds, uint8_t reg) {
    unsigned hour = from_bcd(reg & 0x3F);
    if (!(nds->rtc.stat1 & 2) && (reg & 0x40)) hour += 12;
    return hour % 24;
}

/* Fills the date-time registers (year, month, day, weekday, hour, minute, second). */
static void read_datetime(const Nds *nds, uint8_t out[7]) {
    int64_t t = now_seconds(nds);
    unsigned days = (unsigned)(t / SECONDS_PER_DAY), secs = (unsigned)(t % SECONDS_PER_DAY);
    unsigned year = 2000, month = 1;
    out[3] = (uint8_t)((days + 6) % 7); /* 2000-01-01 was a Saturday */
    while (days >= 365u + leap(year)) days -= 365u + leap(year++);
    while (days >= month_days(year, month)) days -= month_days(year, month++);
    out[0] = bcd(year - 2000);
    out[1] = bcd(month);
    out[2] = bcd(days + 1);
    out[4] = hour_reg(nds, secs / 3600);
    out[5] = bcd(secs / 60 % 60);
    out[6] = bcd(secs % 60);
}

/* Takes date (if `date`) and time registers; out-of-range values are clamped. */
static void write_datetime(Nds *nds, const uint8_t *in, int date) {
    int64_t t = now_seconds(nds), days = t / SECONDS_PER_DAY;
    const uint8_t *time = date ? in + 4 : in;
    unsigned hour = hour_value(nds, time[0]);
    unsigned minute = from_bcd(time[1] & 0x7F) % 60, second = from_bcd(time[2] & 0x7F) % 60;
    if (date) {
        unsigned year = 2000 + from_bcd(in[0]) % 100, month = from_bcd(in[1] & 0x1F);
        unsigned day = from_bcd(in[2] & 0x3F);
        if (month < 1) month = 1;
        if (month > 12) month = 12;
        if (day < 1) day = 1;
        if (day > month_days(year, month)) day = month_days(year, month);
        days = day - 1;
        for (unsigned y = 2000; y < year; y++) days += 365 + leap(y);
        for (unsigned m = 1; m < month; m++) days += month_days(year, m);
    }
    set_seconds(nds, days * SECONDS_PER_DAY + hour * 3600 + minute * 60 + second);
}

static int param_length(const NdsRtc *r) {
    switch (r->command) {
    case CMD_DATETIME: return 7;
    case CMD_TIME: return 3;
    case CMD_INT1: return (r->stat2 & 4) ? 3 : 1; /* alarm 1, or the frequency register */
    case CMD_ALARM2: return 3;
    default: return 1;
    }
}

static void load_reply(Nds *nds) {
    NdsRtc *r = &nds->rtc;
    memset(r->buffer, 0, sizeof r->buffer);
    switch (r->command) {
    case CMD_STAT1:
        r->buffer[0] = r->stat1;
        r->stat1 &= 0x0F; /* the flags clear on read */
        break;
    case CMD_STAT2: r->buffer[0] = r->stat2; break;
    case CMD_DATETIME: read_datetime(nds, r->buffer); break;
    case CMD_TIME: {
        uint8_t dt[7];
        read_datetime(nds, dt);
        memcpy(r->buffer, dt + 4, 3);
        break;
    }
    case CMD_INT1:
        /* The frequency register is the same byte as alarm 1's minute. */
        if (r->stat2 & 4) memcpy(r->buffer, r->alarm1, 3);
        else r->buffer[0] = r->alarm1[2];
        break;
    case CMD_ALARM2: memcpy(r->buffer, r->alarm2, 3); break;
    case CMD_ADJUST: r->buffer[0] = r->adjust; break;
    default: r->buffer[0] = r->free; break;
    }
}

static void reset_clock(Nds *nds) {
    NdsRtc *r = &nds->rtc;
    r->stat1 = 0;
    r->stat2 = 0;
    memset(r->alarm1, 0, sizeof r->alarm1);
    memset(r->alarm2, 0, sizeof r->alarm2);
    r->adjust = 0;
    r->free = 0;
    set_seconds(nds, 0);
}

static void apply_write(Nds *nds) {
    NdsRtc *r = &nds->rtc;
    const uint8_t *b = r->buffer;
    switch (r->command) {
    case CMD_STAT1:
        if (b[0] & 1) reset_clock(nds);
        r->stat1 = (uint8_t)((r->stat1 & 0xF0) | (b[0] & 0x0E));
        break;
    case CMD_STAT2: r->stat2 = b[0]; break;
    case CMD_DATETIME: write_datetime(nds, b, 1); break;
    case CMD_TIME: write_datetime(nds, b, 0); break;
    case CMD_INT1:
        if (r->stat2 & 4) memcpy(r->alarm1, b, 3);
        else r->alarm1[2] = b[0];
        break;
    case CMD_ALARM2: memcpy(r->alarm2, b, 3); break;
    case CMD_ADJUST: r->adjust = b[0]; break;
    default: r->free = b[0]; break;
    }
}

static uint8_t reverse(uint8_t b) {
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

static void byte_done(Nds *nds) {
    NdsRtc *r = &nds->rtc;
    uint8_t b = r->byte;
    r->byte = 0;
    if (!r->have_command) {
        if ((b & 0x0F) != 0x06) {
            if ((b & 0xF0) != 0x60) return; /* not a command: ignore it */
            b = reverse(b);
        }
        r->have_command = 1;
        r->command = (b >> 4) & 7;
        r->reading = b >> 7;
        r->index = 0;
        if (r->reading) load_reply(nds);
        return;
    }
    if (r->index >= param_length(r)) return;
    if (!r->reading) {
        r->buffer[r->index] = b;
        if (r->index + 1 == param_length(r)) apply_write(nds);
    }
    r->index++;
}

void nds_rtc_reset(Nds *nds) {
    NdsRtc *r = &nds->rtc;
    /* The clock has its own battery: the time, and whatever the game set it
     * to, outlive a reset. nds_reset has already folded the elapsed time into
     * `base`, since the cycle count starts over. */
    int64_t base = r->base, offset = r->offset;
    memset(r, 0, sizeof *r);
    r->base = base;
    r->offset = offset;
    r->stat1 = 0x02; /* 24-hour mode */
}

void nds_set_wall_clock(Nds *nds, int64_t unix_seconds) {
    NdsRtc *r = &nds->rtc;
    int64_t since_2000 = unix_seconds - 946684800; /* 2000-01-01 00:00 */
    if (since_2000 < 0) since_2000 = 0;
    /* Line the clock up with the host now, keeping anything the game set. */
    r->base = since_2000 - (int64_t)(nds_now(nds, 1) / NDS_ARM7_HZ);
}

uint32_t nds_rtc_read(Nds *nds) {
    const NdsRtc *r = &nds->rtc;
    uint32_t v = r->pins;
    if (!(r->pins & 0x10)) v = (v & ~1u) | (uint32_t)(r->out & 1); /* SIO is an input */
    return v;
}

void nds_rtc_write(Nds *nds, uint32_t v, uint32_t mask) {
    NdsRtc *r = &nds->rtc;
    uint16_t old = r->pins, now;
    if (!(mask & 0xFFFF)) return;
    now = (uint16_t)((old & ~mask) | (v & mask));
    r->pins = now;
    if (!(old & 4) && (now & 4)) { /* select: a new transfer */
        r->bits = 0;
        r->byte = 0;
        r->index = 0;
        r->have_command = 0;
        r->reading = 0;
        return;
    }
    if (!(now & 4)) {
        r->have_command = 0;
        return;
    }
    if ((old & 2) && !(now & 2) && r->reading && r->have_command) {
        /* Falling edge: the RTC puts the next bit out. */
        int i = r->index < 8 ? r->index : 7;
        r->out = (r->buffer[i] >> r->bits) & 1;
    }
    if (!(old & 2) && (now & 2)) { /* rising edge: a bit is done */
        if (!r->reading || !r->have_command) r->byte |= (uint8_t)((now & 1) << r->bits);
        if (++r->bits == 8) {
            r->bits = 0;
            byte_done(nds);
        }
    }
}
