/* The cartridge's backup chip on the AUXSPI bus (GBATEK, "DS Cartridge Backup",
 * and "DS Firmware Serial Flash Memory" for the FLASH commands).
 *
 * A DS cart doesn't say which chip it carries. The type comes, in order, from
 * the save_type override, from the size of an existing save file, or from how
 * the game talks to the chip. For the last, GBATEK's hint: the SDK sends the
 * command and address bytes from one routine and the data from another, so the
 * bytes written from the first instruction address after chip select are the
 * command plus the address, which gives the address width:
 *
 *   1 byte  0.5 KiB EEPROM (the 9th address bit is in the command)
 *   2 bytes 8 KiB EEPROM, growing to 64 KiB when the game goes past 8 KiB
 *           or writes past a 32-byte page (64 KiB has 128-byte pages)
 *   3 bytes FLASH, 256 KiB, growing to 512 KiB and 1 MiB the same way
 *
 * A detected EEPROM is measured again on every read, since some games
 * (GBATEK: Over the Hedge) first use the wrong width. Writes are measured only
 * while the type is unknown: the SDK can send a write's data from the same
 * routine as its address. A game that asks for the JEDEC ID before anything
 * else is taken to have FLASH.
 *
 * Writes complete at once, so the status register never shows a write in
 * progress. FLASH Page Program (02h) writes rather than ANDs: after an erase
 * the two are the same, and a 128 KiB EEPROM, which looks like FLASH from its
 * address width, writes with 02h too. */

#include "nds_internal.h"

#include <string.h>

typedef struct {
    const char *name;
    uint32_t size;
    uint8_t width; /* address bytes */
    uint8_t flash;
    uint16_t page; /* writes wrap inside a page; 0 = no limit (FRAM) */
} SaveInfo;

static const SaveInfo INFO[NDS_SAVE_TYPE_COUNT] = {
    [NDS_SAVE_AUTO] = {"auto", 0, 0, 0, 0},
    [NDS_SAVE_NONE] = {"none", 0, 0, 0, 0},
    [NDS_SAVE_EEPROM_512] = {"eeprom512", 0x200, 1, 0, 16},
    [NDS_SAVE_EEPROM_8K] = {"eeprom8k", 0x2000, 2, 0, 32},
    [NDS_SAVE_EEPROM_64K] = {"eeprom64k", 0x10000, 2, 0, 128},
    [NDS_SAVE_EEPROM_128K] = {"eeprom128k", 0x20000, 3, 0, 256},
    [NDS_SAVE_FRAM_32K] = {"fram32k", 0x8000, 2, 0, 0},
    [NDS_SAVE_FLASH_256K] = {"flash256k", 0x40000, 3, 1, 256},
    [NDS_SAVE_FLASH_512K] = {"flash512k", 0x80000, 3, 1, 256},
    [NDS_SAVE_FLASH_1M] = {"flash1m", 0x100000, 3, 1, 256},
};

enum { WEL = 2 };

const char *nds_save_type_name(NdsSaveType type) {
    return (unsigned)type < NDS_SAVE_TYPE_COUNT ? INFO[type].name : "";
}

NdsSaveType nds_save_type_from_name(const char *name) {
    for (int t = 0; t < NDS_SAVE_TYPE_COUNT; t++)
        if (name && !strcmp(name, INFO[t].name)) return (NdsSaveType)t;
    return NDS_SAVE_TYPE_COUNT;
}

static int is_eeprom(NdsSaveType t) { return INFO[t].width && !INFO[t].flash; }

/* A new cartridge: a blank chip of unknown type. */
void nds_backup_insert(Nds *nds) {
    NdsBackup *k = &nds->backup;
    NdsSaveType override = k->override;
    memset(k, 0, sizeof *k);
    memset(k->data, 0xFF, sizeof k->data);
    k->override = override;
    k->type = override;
    k->fixed = override != NDS_SAVE_AUTO;
    /* GBATEK: Rune Factory defeats detection; it has a 64 KiB EEPROM. */
    if (!k->fixed && nds->rom && !memcmp(nds->header.game_code, "ARF", 3)) {
        k->type = NDS_SAVE_EEPROM_64K;
        k->fixed = 1;
    }
}

void nds_set_save_type(Nds *nds, NdsSaveType type) {
    NdsBackup *k = &nds->backup;
    if ((unsigned)type >= NDS_SAVE_TYPE_COUNT) return;
    k->override = type;
    if (type != NDS_SAVE_AUTO) {
        /* Data past the new size is dropped; a bigger chip reads FFh there. */
        if (INFO[type].size < sizeof k->data)
            memset(k->data + INFO[type].size, 0xFF, sizeof k->data - INFO[type].size);
        k->type = type;
        k->fixed = 1;
    } else {
        k->fixed = k->from_file;
    }
}

NdsSaveType nds_save_type(const Nds *nds) { return nds->backup.type; }

size_t nds_save_size(const Nds *nds) { return INFO[nds->backup.type].size; }

size_t nds_save_write(const Nds *nds, uint8_t *out, size_t capacity) {
    size_t size = nds_save_size(nds);
    if (!out || capacity < size) return 0;
    memcpy(out, nds->backup.data, size);
    return size;
}

/* A save file's size names its chip. With the override set, the data goes
 * into the chosen chip as far as it fits. */
int nds_save_load(Nds *nds, const uint8_t *data, size_t size) {
    NdsBackup *k = &nds->backup;
    NdsSaveType type = NDS_SAVE_TYPE_COUNT;
    if (!data || !size) return 0;
    if (k->override != NDS_SAVE_AUTO) {
        type = k->override;
        if (!INFO[type].size) return 0;
        if (size > INFO[type].size) size = INFO[type].size;
    } else {
        for (int t = 0; t < NDS_SAVE_TYPE_COUNT; t++)
            if (INFO[t].size == size) type = (NdsSaveType)t;
        if (type == NDS_SAVE_TYPE_COUNT) return 0;
    }
    memset(k->data, 0xFF, sizeof k->data);
    memcpy(k->data, data, size);
    k->type = type;
    k->fixed = 1;
    k->from_file = k->override == NDS_SAVE_AUTO;
    k->dirty = 0;
    return 1;
}

int nds_save_dirty(const Nds *nds) { return nds->backup.dirty && nds_save_size(nds) != 0; }

void nds_save_mark_clean(Nds *nds) { nds->backup.dirty = 0; }

/* Detection settled on `width` address bytes. */
static void detected(NdsBackup *k, int width) {
    NdsSaveType t = k->type;
    if (width == 1) t = NDS_SAVE_EEPROM_512;
    else if (width == 2) t = (t == NDS_SAVE_EEPROM_64K) ? t : NDS_SAVE_EEPROM_8K;
    else if (width >= 3) t = (t == NDS_SAVE_AUTO) ? NDS_SAVE_FLASH_256K : is_eeprom(t) ? NDS_SAVE_EEPROM_128K : t;
    k->type = t;
}

/* A chip grows to cover the addresses the game uses, unless the override
 * named it. One sized by a save file grows too: the file may come from an
 * earlier run whose detection stopped short. */
static void cover(NdsBackup *k, uint32_t address) {
    if (k->override != NDS_SAVE_AUTO) return;
    while (address >= INFO[k->type].size) {
        if (k->type == NDS_SAVE_EEPROM_8K) k->type = NDS_SAVE_EEPROM_64K;
        else if (k->type == NDS_SAVE_FLASH_256K) k->type = NDS_SAVE_FLASH_512K;
        else if (k->type == NDS_SAVE_FLASH_512K) k->type = NDS_SAVE_FLASH_1M;
        else break;
    }
}

static int reads(uint8_t c) { return c == 0x03 || c == 0x0B; }
static int writes(uint8_t c) { return c == 0x02 || c == 0x0A; }
static int erases(uint8_t c) { return c == 0xDB || c == 0xD8; }

/* The first byte after chip select. */
static void begin(NdsBackup *k, uint8_t command, uint32_t pc) {
    k->command = command;
    k->step = 0;
    k->address = 0;
    k->seen = 0;
    k->pc = pc;
    k->in_data = 0;
    k->dummy = 0;
    k->measuring = 0;
    k->wrapped = 0;
    if (command == 0x06) k->status |= WEL;
    else if (command == 0x04) k->status &= (uint8_t)~WEL;
    else if (command == 0x9F && k->type == NDS_SAVE_AUTO) k->type = NDS_SAVE_FLASH_256K;
    /* Only reads re-measure a known EEPROM: the SDK sends a write's data from
     * the same routine as its address (seen in Assassin's Creed II:
     * Discovery), so a write's run says nothing about the width. */
    if (!k->fixed && (k->type == NDS_SAVE_AUTO ? reads(command) || writes(command) || erases(command)
                                               : is_eeprom(k->type) && reads(command)))
        k->measuring = 1;
}

static void address_done(NdsBackup *k);

/* The measured run of address bytes ended; settle the type and the address. */
static void measured(NdsBackup *k) {
    int dummy_sent = 0;
    /* A FAST READ's dummy byte can travel with the address. */
    if (k->seen == 4 && k->command == 0x0B) {
        k->address >>= 8;
        k->seen = 3;
        dummy_sent = 1;
    }
    detected(k, (int)k->seen);
    if (INFO[k->type].width < k->seen) k->address &= (1u << (8 * INFO[k->type].width)) - 1;
    k->measuring = 0;
    address_done(k);
    if (dummy_sent) k->dummy = 0;
}

/* The address is complete: set up the data phase. */
static void address_done(NdsBackup *k) {
    const SaveInfo *info = &INFO[k->type];
    /* The 0.5 KiB EEPROM's 0Bh/0Ah reach its upper 256 bytes. */
    if (k->type == NDS_SAVE_EEPROM_512 && (k->command == 0x0B || k->command == 0x0A)) k->address |= 0x100;
    cover(k, k->address);
    if (info->flash && k->command == 0x0B) k->dummy = 1; /* FAST READ's dummy byte */
    if (erases(k->command) && info->flash && (k->status & WEL)) {
        uint32_t span = k->command == 0xD8 ? 0x10000u : 0x100u, size = INFO[k->type].size;
        uint32_t start = (k->address & ~(span - 1)) % size;
        memset(k->data + start, 0xFF, span < size ? span : size);
        k->dirty = 1;
    }
    k->in_data = 1;
}

static uint8_t data_byte(NdsBackup *k, uint8_t b) {
    const SaveInfo *info = &INFO[k->type];
    uint32_t a;
    if (k->dummy) {
        k->dummy = 0;
        return 0xFF;
    }
    if (!info->size) return 0xFF;
    a = k->address % info->size;
    if (reads(k->command)) {
        k->address = (a + 1) % info->size;
        return k->data[a];
    }
    if (writes(k->command) && (k->status & WEL)) {
        /* A write running on past an 8 KiB EEPROM's 32-byte page means
         * 128-byte pages: the chip is 64 KiB, which takes the same two address
         * bytes. The byte goes where the wrap would have hidden it. */
        if (k->wrapped && k->type == NDS_SAVE_EEPROM_8K && k->override == NDS_SAVE_AUTO) {
            k->type = NDS_SAVE_EEPROM_64K;
            info = &INFO[k->type];
            a += 32;
        }
        k->wrapped = 0;
        if (k->data[a] != b) k->dirty = 1;
        k->data[a] = b;
        if (info->page && !((a + 1) & (info->page - 1u))) k->wrapped = 1;
        if (info->page) k->address = (a & ~(uint32_t)(info->page - 1)) | ((a + 1) & (info->page - 1u));
        else k->address = (a + 1) % info->size;
    }
    return 0xFF;
}

static uint8_t transfer(Nds *nds, uint8_t b, uint32_t pc) {
    NdsBackup *k = &nds->backup;
    const SaveInfo *info;
    if (k->type == NDS_SAVE_NONE) return 0xFF;
    if (!k->selected) {
        k->selected = 1;
        begin(k, b, pc);
        return 0xFF;
    }
    k->step++;
    info = &INFO[k->type];
    switch (k->command) {
    case 0x05: /* RDSR, repeated */
        return k->type == NDS_SAVE_EEPROM_512 ? (uint8_t)(k->status | 0xF0) : k->status;
    case 0x01: /* WRSR: only the protect bits (and SRWD) change */
        if (k->step == 1 && (k->status & WEL)) {
            k->status = (uint8_t)((k->status & 3) | (b & 0x8C));
        }
        return 0xFF;
    case 0x9F: { /* RDID: FLASH answers with a JEDEC ID (GBATEK's ST parts) */
        static const uint8_t id[3][3] = {{0x20, 0x40, 0x12}, {0x20, 0x40, 0x13}, {0x20, 0x40, 0x14}};
        if (!info->flash || k->step > 3) return 0xFF;
        return id[k->type - NDS_SAVE_FLASH_256K][k->step - 1];
    }
    default:
        break;
    }
    if (!reads(k->command) && !writes(k->command) && !erases(k->command)) return 0xFF;
    if (!k->in_data) {
        if (k->measuring) {
            if (pc == k->pc && k->seen < 4) {
                k->address = k->address << 8 | b;
                k->seen++;
                return 0xFF;
            }
            if (k->seen == 0) {
                /* Command and address came from different routines: measure
                 * the address's own run instead. */
                k->pc = pc;
                k->address = b;
                k->seen = 1;
                return 0xFF;
            }
            measured(k);
            return data_byte(k, b);
        }
        k->address = k->address << 8 | b;
        if (++k->seen < info->width) return 0xFF;
        address_done(k);
        return 0xFF;
    }
    return data_byte(k, b);
}

/* One AUXSPIDATA write in backup mode. `last` is set when chip select drops
 * after this byte (AUXSPICNT bit 6 clear). */
uint8_t nds_backup_transfer(Nds *nds, int cpu, uint8_t b, int last) {
    NdsBackup *k = &nds->backup;
    uint8_t out = transfer(nds, b, nds_arm_pc(cpu ? &nds->arm7 : &nds->arm9));    if (last && k->selected) {
        /* An erase sends nothing after its address, so its measured run ends
         * with chip select. */
        if (k->measuring && !k->in_data && k->seen) measured(k);
        /* A finished write, erase or WRSR clears the write enable latch. */
        if ((writes(k->command) || erases(k->command) || k->command == 0x01) && k->step)
            k->status &= (uint8_t)~WEL;
        k->selected = 0;
    }
    return out;
}

/* Chip select drops when the slot leaves SPI mode or is disabled. */
void nds_backup_deselect(Nds *nds) { nds->backup.selected = 0; }
