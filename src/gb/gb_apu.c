/* Audio processing unit: two square channels (one with sweep), a wave channel,
 * and a noise channel, mixed to 48 kHz stereo. The 512 Hz frame sequencer is
 * driven by the DIV counter, as on hardware. */

#include "gb_internal.h"

#include <string.h>

#define REG(a) (g->apu.regs[(a) - 0xFF10])

static const uint8_t read_masks[0x20] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF, /* NR10-NR14 */
    0xFF, 0x3F, 0x00, 0xFF, 0xBF, /* --, NR21-NR24 */
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF, /* NR30-NR34 */
    0xFF, 0xFF, 0x00, 0x00, 0xBF, /* --, NR41-NR44 */
    0x00, 0x00, 0x70,             /* NR50-NR52 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static const uint8_t duty_table[4] = {0x01, 0x81, 0x87, 0x7E};

static int square_frequency(Gb *g, int n) {
    uint16_t base = n == 0 ? 0xFF13 : 0xFF18;
    return REG(base) | ((REG(base + 1) & 7) << 8);
}

static int channel_period(Gb *g, int n) {
    switch (n) {
    case 0:
    case 1: return (2048 - square_frequency(g, n)) * 4;
    case 2: return (2048 - (REG(0xFF1D) | ((REG(0xFF1E) & 7) << 8))) * 2;
    default: {
        uint8_t nr43 = REG(0xFF22);
        int divisor = (nr43 & 7) ? (nr43 & 7) * 16 : 8;
        return divisor << (nr43 >> 4);
    }
    }
}

static int sweep_calculate(Gb *g) {
    GbChannel *ch = &g->apu.ch[0];
    int delta = ch->sweep_shadow >> ch->sweep_shift;
    int value;
    if (ch->sweep_negate) {
        value = ch->sweep_shadow - delta;
        ch->sweep_negated_used = 1;
    } else {
        value = ch->sweep_shadow + delta;
    }
    if (value > 2047) ch->active = 0;
    return value;
}

static void trigger(Gb *g, int n) {
    GbApu *apu = &g->apu;
    GbChannel *ch = &apu->ch[n];
    int max_length = n == 2 ? 256 : 64;
    ch->active = ch->enabled;
    if (ch->length == 0) {
        ch->length = max_length;
        if (ch->length_enabled && (apu->frame_step & 1)) ch->length--;
    }
    ch->timer = channel_period(g, n);
    if (n == 2) {
        ch->position = 0;
        ch->timer += 6;
        return;
    }
    {
        uint8_t env = REG(n == 0 ? 0xFF12 : n == 1 ? 0xFF17 : 0xFF21);
        ch->volume = env >> 4;
        ch->env_up = (env >> 3) & 1;
        ch->env_period = env & 7;
        ch->env_timer = ch->env_period ? ch->env_period : 8;
    }
    if (n == 3) ch->lfsr = 0x7FFF;
    if (n == 0) {
        uint8_t nr10 = REG(0xFF10);
        ch->sweep_period = (nr10 >> 4) & 7;
        ch->sweep_negate = (nr10 >> 3) & 1;
        ch->sweep_shift = nr10 & 7;
        ch->sweep_shadow = square_frequency(g, 0);
        ch->sweep_timer = ch->sweep_period ? ch->sweep_period : 8;
        ch->sweep_enabled = ch->sweep_period || ch->sweep_shift;
        ch->sweep_negated_used = 0;
        if (ch->sweep_shift) sweep_calculate(g);
    }
}

static void write_length_control(Gb *g, int n, uint8_t v) {
    GbApu *apu = &g->apu;
    GbChannel *ch = &apu->ch[n];
    int was_enabled = ch->length_enabled;
    ch->length_enabled = (v >> 6) & 1;
    /* Enabling the length counter in the half of the sequencer period that does not
     * clock it still clocks it once. */
    if (!was_enabled && ch->length_enabled && (apu->frame_step & 1) && ch->length > 0) {
        ch->length--;
        if (ch->length == 0 && !(v & 0x80)) ch->active = 0;
    }
    if (v & 0x80) trigger(g, n);
}

void gb_apu_reset(Gb *g) {
    GbApu *apu = &g->apu;
    memset(apu, 0, sizeof *apu);
    apu->powered = 1;
    /* State after the boot ROM's chime. */
    REG(0xFF10) = 0x80; REG(0xFF11) = 0xBF; REG(0xFF12) = 0xF3; REG(0xFF14) = 0xBF;
    REG(0xFF16) = 0x3F; REG(0xFF19) = 0xBF; REG(0xFF1A) = 0x7F; REG(0xFF1B) = 0xFF;
    REG(0xFF1C) = 0x9F; REG(0xFF1E) = 0xBF; REG(0xFF20) = 0xFF; REG(0xFF23) = 0xBF;
    REG(0xFF24) = 0x77; REG(0xFF25) = 0xF3;
    apu->ch[0].enabled = 1;
    apu->ch[0].active = 1;
    apu->ch[0].duty = 2;
    apu->ch[3].lfsr = 0x7FFF;
}

void gb_apu_frame_sequencer(Gb *g) {
    GbApu *apu = &g->apu;
    int step;
    int n;
    if (!apu->powered) return;
    step = apu->frame_step;
    apu->frame_step = (step + 1) & 7;

    if ((step & 1) == 0) {
        for (n = 0; n < 4; n++) {
            GbChannel *ch = &apu->ch[n];
            if (ch->length_enabled && ch->length > 0) {
                ch->length--;
                if (ch->length == 0) ch->active = 0;
            }
        }
    }
    if (step == 2 || step == 6) {
        GbChannel *ch = &apu->ch[0];
        if (ch->sweep_timer > 0) ch->sweep_timer--;
        if (ch->sweep_timer == 0) {
            ch->sweep_timer = ch->sweep_period ? ch->sweep_period : 8;
            if (ch->sweep_enabled && ch->sweep_period) {
                int value = sweep_calculate(g);
                if (value <= 2047 && ch->sweep_shift) {
                    ch->sweep_shadow = value;
                    REG(0xFF13) = (uint8_t)value;
                    REG(0xFF14) = (uint8_t)((REG(0xFF14) & 0xF8) | ((value >> 8) & 7));
                    sweep_calculate(g);
                }
            }
        }
    }
    if (step == 7) {
        for (n = 0; n < 4; n++) {
            GbChannel *ch;
            if (n == 2) continue;
            ch = &apu->ch[n];
            if (ch->env_period == 0) continue;
            if (--ch->env_timer <= 0) {
                ch->env_timer = ch->env_period;
                if (ch->env_up && ch->volume < 15) ch->volume++;
                else if (!ch->env_up && ch->volume > 0) ch->volume--;
            }
        }
    }
}

static int channel_output(Gb *g, int n) {
    GbChannel *ch = &g->apu.ch[n];
    int sample;
    if (!ch->enabled) return 0;
    if (!ch->active) return -15; /* DAC on, channel silent: digital 0 */
    switch (n) {
    case 0:
    case 1: {
        int duty = REG(n == 0 ? 0xFF11 : 0xFF16) >> 6;
        sample = ((duty_table[duty] >> (7 - ch->position)) & 1) ? ch->volume : 0;
        break;
    }
    case 2: {
        static const int shifts[4] = {4, 0, 1, 2};
        uint8_t byte = REG(0xFF30 + ch->position / 2);
        int nibble = (ch->position & 1) ? (byte & 0x0F) : (byte >> 4);
        sample = nibble >> shifts[(REG(0xFF1C) >> 5) & 3];
        break;
    }
    default:
        sample = (ch->lfsr & 1) ? 0 : ch->volume;
        break;
    }
    return sample * 2 - 15; /* DAC: digital 0..15 to roughly -1..+1 */
}

static void push_sample(GbApu *apu, int16_t left, int16_t right) {
    size_t next = (apu->write_pos + 1) & (GB_AUDIO_CAPACITY - 1);
    if (next == apu->read_pos) {
        apu->read_pos = (apu->read_pos + 1) & (GB_AUDIO_CAPACITY - 1); /* drop the oldest */
    }
    apu->buffer[apu->write_pos * 2] = left;
    apu->buffer[apu->write_pos * 2 + 1] = right;
    apu->write_pos = next;
}

void gb_apu_step(Gb *g, int cycles) {
    GbApu *apu = &g->apu;
    int n;
    int left = 0;
    int right = 0;

    if (apu->powered) {
        for (n = 0; n < 4; n++) {
            GbChannel *ch = &apu->ch[n];
            if (!ch->active) continue;
            ch->timer -= cycles;
            while (ch->timer <= 0) {
                ch->timer += channel_period(g, n);
                if (n == 3) {
                    int bit = (ch->lfsr ^ (ch->lfsr >> 1)) & 1;
                    ch->lfsr = (uint16_t)((ch->lfsr >> 1) | (bit << 14));
                    if (REG(0xFF22) & 0x08) ch->lfsr = (uint16_t)((ch->lfsr & ~0x40) | (bit << 6));
                } else {
                    ch->position = (ch->position + 1) & (n == 2 ? 31 : 7);
                }
            }
        }
        {
            uint8_t nr51 = REG(0xFF25);
            uint8_t nr50 = REG(0xFF24);
            for (n = 0; n < 4; n++) {
                int out = channel_output(g, n);
                if (nr51 & (0x10 << n)) left += out;
                if (nr51 & (0x01 << n)) right += out;
            }
            left *= ((nr50 >> 4) & 7) + 1;
            right *= (nr50 & 7) + 1;
        }
    }

    apu->acc_left += left * cycles;
    apu->acc_right += right * cycles;
    apu->acc_count += (uint32_t)cycles;
    apu->sample_phase += (uint32_t)cycles * GB_AUDIO_SAMPLE_RATE;
    if (apu->sample_phase >= GB_CLOCK_HZ) {
        /* Peak per side is 4 channels * 15 * 8 = 480; leave headroom. */
        double scale = 32767.0 / 480.0 * 0.7;
        double l = (double)apu->acc_left / apu->acc_count * scale;
        double r = (double)apu->acc_right / apu->acc_count * scale;
        double out_l = l - apu->hp_left;
        double out_r = r - apu->hp_right;
        /* High-pass "capacitor" removes the DAC's DC offset, as on hardware. */
        apu->hp_left = l - out_l * 0.996;
        apu->hp_right = r - out_r * 0.996;
        if (out_l > 32767) out_l = 32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r > 32767) out_r = 32767;
        if (out_r < -32768) out_r = -32768;
        push_sample(apu, (int16_t)out_l, (int16_t)out_r);
        apu->sample_phase -= GB_CLOCK_HZ;
        apu->acc_left = 0;
        apu->acc_right = 0;
        apu->acc_count = 0;
    }
}

uint8_t gb_apu_read(Gb *g, uint16_t a) {
    GbApu *apu = &g->apu;
    if (a >= 0xFF30) return REG(a);
    if (a == 0xFF26) {
        uint8_t v = (uint8_t)(0x70 | (apu->powered ? 0x80 : 0));
        int n;
        for (n = 0; n < 4; n++) if (apu->ch[n].active) v |= (uint8_t)(1u << n);
        return v;
    }
    return (uint8_t)(REG(a) | read_masks[a - 0xFF10]);
}

void gb_apu_write(Gb *g, uint16_t a, uint8_t v) {
    GbApu *apu = &g->apu;
    if (a >= 0xFF30) { REG(a) = v; return; }
    if (a == 0xFF26) {
        if (apu->powered && !(v & 0x80)) {
            int n;
            for (n = 0xFF10; n < 0xFF26; n++) REG(n) = 0;
            for (n = 0; n < 4; n++) {
                int length = apu->ch[n].length; /* DMG keeps length counters while off */
                memset(&apu->ch[n], 0, sizeof apu->ch[n]);
                apu->ch[n].length = length;
            }
            apu->powered = 0;
        } else if (!apu->powered && (v & 0x80)) {
            apu->powered = 1;
            apu->frame_step = 0;
        }
        return;
    }
    if (!apu->powered) {
        /* Only length loads work while powered off on DMG. */
        if (a == 0xFF11) apu->ch[0].length = 64 - (v & 63);
        else if (a == 0xFF16) apu->ch[1].length = 64 - (v & 63);
        else if (a == 0xFF1B) apu->ch[2].length = 256 - v;
        else if (a == 0xFF20) apu->ch[3].length = 64 - (v & 63);
        return;
    }

    REG(a) = v;
    switch (a) {
    case 0xFF10:
        if (apu->ch[0].sweep_negated_used && !(v & 0x08)) apu->ch[0].active = 0;
        apu->ch[0].sweep_period = (v >> 4) & 7;
        apu->ch[0].sweep_negate = (v >> 3) & 1;
        apu->ch[0].sweep_shift = v & 7;
        break;
    case 0xFF11: apu->ch[0].length = 64 - (v & 63); break;
    case 0xFF16: apu->ch[1].length = 64 - (v & 63); break;
    case 0xFF1B: apu->ch[2].length = 256 - v; break;
    case 0xFF20: apu->ch[3].length = 64 - (v & 63); break;
    case 0xFF12: case 0xFF17: case 0xFF21: {
        int n = a == 0xFF12 ? 0 : a == 0xFF17 ? 1 : 3;
        apu->ch[n].enabled = (v & 0xF8) != 0;
        if (!apu->ch[n].enabled) apu->ch[n].active = 0;
        break;
    }
    case 0xFF1A:
        apu->ch[2].enabled = (v & 0x80) != 0;
        if (!apu->ch[2].enabled) apu->ch[2].active = 0;
        break;
    case 0xFF14: write_length_control(g, 0, v); break;
    case 0xFF19: write_length_control(g, 1, v); break;
    case 0xFF1E: write_length_control(g, 2, v); break;
    case 0xFF23: write_length_control(g, 3, v); break;
    default: break;
    }
}
