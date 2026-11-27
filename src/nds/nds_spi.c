/* The ARM7's SPI bus and its three devices (GBATEK, "DS Serial Peripheral
 * Interface Bus (SPI)", "DS Firmware Serial Flash Memory", "DS Touch Screen
 * Controller (TSC)", "DS Power Management Device").
 *
 * The firmware flash holds a generated image rather than a dump: a header, a
 * wifi block and two copies of the user settings, enough for games that read
 * their settings, calibration or MAC address. Writes change only the image in
 * memory. Transfers finish at once, so SPICNT never reads busy. */

#include "nds_internal.h"

#include <string.h>

enum { DEV_POWER, DEV_FIRMWARE, DEV_TOUCH, DEV_RESERVED };

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* One user settings area (GBATEK, "DS Firmware User Settings"). */
static void build_user(uint8_t *u, uint16_t counter) {
    static const char nickname[] = "Nacre";
    memset(u, 0, 0x100);
    put16(u + 0x00, 5);   /* version */
    u[0x02] = 0;          /* favourite colour */
    u[0x03] = 1;          /* birthday: 1 January */
    u[0x04] = 1;
    for (size_t i = 0; i < sizeof nickname - 1; i++) put16(u + 0x06 + i * 2, (uint8_t)nickname[i]);
    put16(u + 0x1A, (uint16_t)(sizeof nickname - 1));
    /* Touch calibration: ADC 200h,200h at pixel (21h,21h) and E00h,A00h at
     * (E1h,A1h), so a pixel's ADC value is simply 16 times its coordinate. */
    put16(u + 0x58, 0x200);
    put16(u + 0x5A, 0x200);
    u[0x5C] = 0x21;
    u[0x5D] = 0x21;
    put16(u + 0x5E, 0xE00);
    put16(u + 0x60, 0xA00);
    u[0x62] = 0xE1;
    u[0x63] = 0xA1;
    /* English, with every "settings okay" bit, so nothing prompts for setup. */
    put16(u + 0x64, 0xEC01);
    u[0x66] = 26; /* year last set, from 2000 */
    memset(u + 0x6C, 0xFF, 4);
    put16(u + 0x70, counter);
    put16(u + 0x72, nds_crc16(0xFFFF, u, 0x70));
    memset(u + 0x74, 0xFF, 0x8C);
}

void nds_firmware_build(Nds *nds) {
    uint8_t *fw = nds->firmware;
    static const uint8_t timestamp[5] = {0x00, 0x12, 0x01, 0x01, 0x06}; /* BCD min hour day month year */
    static const uint8_t mac[6] = {0x00, 0x09, 0xBF, 0x00, 0x00, 0x01};
    memset(fw, 0xFF, NDS_FIRMWARE_SIZE);
    /* Header (GBATEK, "DS Firmware Header"). The boot code parts are absent. */
    memset(fw, 0, 0x2A);
    memcpy(fw + 0x08, "MAC", 3);
    fw[0x0B] = 0x68;
    put16(fw + 0x14, 2u << 12); /* 256 KiB */
    memcpy(fw + 0x18, timestamp, 5);
    fw[0x1D] = 0xFF;            /* original DS */
    put16(fw + 0x1E, 0xFFFF);
    put16(fw + 0x20, NDS_FIRMWARE_USER / 8);
    put16(fw + 0x22, 0x7EC0);
    put16(fw + 0x24, 0x7E40);
    put16(fw + 0x28, 0xFFFF);
    /* Wifi calibration (GBATEK, "DS Firmware Wifi Calibration Data"): the
     * identity fields; the radio values stay zero until wifi exists. */
    memset(fw + 0x2C, 0, 0x138);
    put16(fw + 0x2C, 0x138);
    memcpy(fw + 0x36, mac, 6);
    put16(fw + 0x3C, 0x3FFE); /* channels 1-13 */
    put16(fw + 0x3E, 0xFFFF);
    fw[0x40] = 2;
    fw[0x41] = 0x18;
    fw[0x42] = 0x0C;
    fw[0x43] = 1;
    put16(fw + 0x2A, nds_crc16(0, fw + 0x2C, 0x138));
    /* Access points 1-3 stay FFh-filled: none configured. Both settings areas
     * hold the same settings; the second, one count newer, wins. */
    build_user(fw + NDS_FIRMWARE_USER, 0);
    build_user(fw + NDS_FIRMWARE_USER + 0x100, 1);
}

void nds_spi_reset(Nds *nds) {
    NdsSpi *s = &nds->spi;
    memset(s, 0, sizeof *s);
    s->pm_regs[0] = 0x0D; /* sound amplifier and both backlights on */
}

/* ---- Firmware flash ---- */

static int flash_writes(uint8_t command) {
    return command == 0x0A || command == 0x02 || command == 0xDB || command == 0xD8;
}

static uint8_t flash_byte(Nds *nds, uint8_t b, int first) {
    NdsSpi *s = &nds->spi;
    uint8_t *fw = nds->firmware;
    uint32_t step;
    if (first) {
        s->fw_command = b;
        s->fw_step = 0;
        s->fw_address = 0;
        if (b == 0x06) s->fw_status |= 2;              /* WREN */
        else if (b == 0x04) s->fw_status &= (uint8_t)~2; /* WRDI */
        return 0xFF;
    }
    step = ++s->fw_step;
    switch (s->fw_command) {
    case 0x03: /* READ */
    case 0x0B: /* FAST READ, with a dummy byte after the address */
        if (step <= 3) {
            s->fw_address = (s->fw_address << 8 | b) & (NDS_FIRMWARE_SIZE - 1);
            return 0xFF;
        }
        if (s->fw_command == 0x0B && step == 4) return 0xFF;
        b = fw[s->fw_address];
        s->fw_address = (s->fw_address + 1) & (NDS_FIRMWARE_SIZE - 1);
        return b;
    case 0x05: /* RDSR, repeated */
        return s->fw_status;
    case 0x9F: { /* RDID: ST M45PE20 */
        static const uint8_t id[3] = {0x20, 0x40, 0x12};
        return step <= 3 ? id[step - 1] : 0xFF;
    }
    case 0x0A: /* PW: write within the page */
    case 0x02: /* PP: program, which can only clear bits */
    case 0xDB: /* PE: erase a page */
    case 0xD8: /* SE: erase a 64 KiB sector */
        if (step <= 3) {
            s->fw_address = (s->fw_address << 8 | b) & (NDS_FIRMWARE_SIZE - 1);
            if (step == 3 && (s->fw_status & 2)) {
                if (s->fw_command == 0xDB) memset(fw + (s->fw_address & ~0xFFu), 0xFF, 0x100);
                if (s->fw_command == 0xD8) memset(fw + (s->fw_address & ~0xFFFFu), 0xFF, 0x10000);
            }
            return 0xFF;
        }
        if ((s->fw_command == 0x0A || s->fw_command == 0x02) && (s->fw_status & 2)) {
            uint32_t a = s->fw_address;
            fw[a] = s->fw_command == 0x0A ? b : (uint8_t)(fw[a] & b);
            s->fw_address = (a & ~0xFFu) | ((a + 1) & 0xFFu);
        }
        return 0xFF;
    default:
        return 0xFF;
    }
}

/* ---- Touchscreen controller ---- */

/* A conversion result, 12 bits. With the calibration above, a pixel's ADC
 * value is 16 times its coordinate. */
static uint32_t tsc_convert(const Nds *nds, unsigned channel) {
    switch (channel) {
    case 0: return 0x5D0;                                   /* temperature 0 */
    case 1: return nds->touching ? (uint32_t)nds->touch_y * 16u : 0xFFF; /* Y */
    case 3: return nds->touching ? 0x400 : 0;               /* Z1 */
    case 4: return nds->touching ? 0xC00 : 0xFFF;           /* Z2 */
    case 5: return nds->touching ? (uint32_t)nds->touch_x * 16u : 0; /* X */
    case 6: return 0x800;                                   /* microphone at rest */
    case 7: return 0x5D0 + 142;                             /* temperature 1, about 25 C */
    default: return 0;                                      /* battery: grounded */
    }
}

/* The reply is a zero bit, then the result MSB first, then zeros. A control
 * byte (bit 7 set) starts a new conversion while the old reply shifts out. */
static uint8_t touch_byte(Nds *nds, uint8_t b) {
    NdsSpi *s = &nds->spi;
    uint8_t reply = (uint8_t)(s->tsc_out >> 8);
    if (b & 0x80) {
        uint32_t v = tsc_convert(nds, (b >> 4) & 7);
        s->tsc_out = (b & 0x08) ? (uint16_t)((v >> 4) << 7) : (uint16_t)(v << 3);
    } else {
        s->tsc_out = (uint16_t)(s->tsc_out << 8);
    }
    return reply;
}

/* ---- Power management ---- */

static uint8_t power_byte(Nds *nds, uint8_t b, int first) {
    static const uint8_t writable[4] = {0x7F, 0x00, 0x01, 0x03};
    NdsSpi *s = &nds->spi;
    unsigned reg;
    if (first) {
        s->pm_index = b;
        s->pm_step = 0;
        return 0;
    }
    if (s->pm_step++) return 0;
    reg = s->pm_index & 3; /* registers 4-7Fh mirror 0-3 on the original DS */
    if (s->pm_index & 0x80) return s->pm_regs[reg];
    s->pm_regs[reg] = (uint8_t)((s->pm_regs[reg] & ~writable[reg]) | (b & writable[reg]));
    return 0;
}

/* ---- The bus ---- */

static void deselect(Nds *nds) {
    NdsSpi *s = &nds->spi;
    if (s->active && s->device == DEV_FIRMWARE && flash_writes(s->fw_command))
        s->fw_status &= (uint8_t)~2; /* WEL clears when a write or erase completes */
    s->active = 0;
}

static uint8_t transfer(Nds *nds, uint8_t b) {
    NdsSpi *s = &nds->spi;
    int device = (s->control >> 8) & 3, first = 0;
    if (!s->active || s->device != device) {
        if (s->active) deselect(nds);
        s->active = 1;
        s->device = device;
        first = 1;
        if (device == DEV_TOUCH) s->tsc_out = 0;
    }
    switch (device) {
    case DEV_POWER: return power_byte(nds, b, first);
    case DEV_FIRMWARE: return flash_byte(nds, b, first);
    case DEV_TOUCH: return touch_byte(nds, b);
    default: return 0xFF;
    }
}

/* SPICNT in the low half, SPIDATA in the high half. */
uint32_t nds_spi_read(Nds *nds) {
    return nds->spi.control | (uint32_t)nds->spi.data << 16;
}

void nds_spi_write(Nds *nds, uint32_t v, uint32_t mask) {
    NdsSpi *s = &nds->spi;
    if (mask & 0xFFFF) {
        s->control = (uint16_t)((s->control & ~(mask & 0xCF03u)) | (v & mask & 0xCF03u));
        if (!(s->control & 0x8000)) deselect(nds);
    }
    if ((mask & 0x00FF0000u) && (s->control & 0x8000)) {
        s->data = transfer(nds, (uint8_t)(v >> 16));
        if (!(s->control & 0x0800)) deselect(nds);
        if (s->control & 0x4000) nds_irq_raise(nds, 1, NDS_IRQ_SPI);
    }
}
