/*
 * config_store.h: stored settings, per slot (research/core/presets-design.md
 * §4 and §10.3). Implemented per platform:
 *   - NUC1xx data-flash journal (platform/nuvoton-nuc1xx/src/config_store.c)
 *   - ESP32 NVS / ESP8266 EEPROM sector (platform/esp/config_store_esp.cpp)
 *
 * The store is dumb: it keeps tagged records and never interprets values.
 * The core (src/presets.c) decides what goes in a record: for slot 0 the
 * global params, for slot n the per-preset values that differ from preset
 * n's YAML values. The newest record of a slot wins.
 *
 * Record v2, in 32-bit words (NUC journal; the ESP stores the same words):
 *
 *   magic 'GCF2' (0x32464347), seq,
 *   slot (bits 0-7) | flags (bits 8-15) | revision (bits 16-31),
 *   count,
 *   name: 4 words (16 bytes of ASCII, NUL-padded), only if flags & CS_F_NAME,
 *   count x { id | index << 16, value },
 *   crc32 (zlib) over all the words before it
 *
 * Flags: CS_F_NAME (a name follows), CS_F_TOMBSTONE (the slot was reset: no
 * stored values; an older record of the slot no longer counts).
 *
 * Old v1 records (magic 'GCFG', no slot) still load: they read as slot 0
 * and slot 1 at once (every value they hold is applied to whichever of
 * the two the param belongs to). The next write produces v2. (NUC: once
 * only one of the two still reads it, compaction copies it as a v2 record
 * of that slot; a 62-entry v1 record then keeps its first 61 entries.)
 *
 * Power cuts: the newest record of every slot is never erased before a
 * newer copy of it exists (NUC: compaction copies live records forward
 * before a page is erased; ESP32: NVS writes are atomic; ESP8266: the one
 * EEPROM sector is rewritten, so a cut there can lose everything).
 *
 * Capacity (§10.5): config_store_max_entries() is 59 on the NUC (a named
 * record fills one 512 B page: 4 + 4 + 2 x 59 + 1 = 127 words), 243 on
 * ESP32 (a 1984 B blob), 507 on ESP8266 (the 4 KB sector, shared by all
 * slots). NUC journal: 7 pages; the worst-case records of all slots,
 * packed whole into pages, must fit in 5 (one page is the erase spare,
 * one is room for a slot's old and new copy while it is rewritten).
 *
 * NUC1xx: needs fmc_open() first. Erases stall the CPU (about 20-40 ms per page).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_CONFIG_STORE_H
#define GEMIFI_CONFIG_STORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_STORE_MAGIC    0x47464347u   /* 'GCFG': v1 record (read only) */
#define CONFIG_STORE_MAGIC_V2 0x32464347u   /* 'GCF2' */

#define CS_SLOT_GLOBAL 0u                   /* slots 1..GM_N_PRESETS = presets */
#define CS_NAME_LEN    16u

#define CS_F_NAME      0x01u
#define CS_F_TOMBSTONE 0x02u

typedef struct {
    uint16_t id;
    uint16_t index;
    int32_t value;
} cs_entry_t;

typedef struct {
    uint16_t revision;                     /* presets: { revision } it was saved under */
    uint8_t flags;                         /* CS_F_*                              */
    char name[CS_NAME_LEN + 1];            /* NUL-terminated; "" if no CS_F_NAME  */
} cs_slot_info_t;

/* Write a new record for `slot` (replaces the previous one). name may be
 * NULL (no name stored). Returns GSX_OK or GSX_FLASH_ERR (GSX_BAD_LEN if
 * n entries can't fit one record on this platform). */
int config_store_write(uint8_t slot, uint16_t revision, const char *name,
                       const cs_entry_t *e, uint32_t n);

/* Find the newest record of `slot`. If there is one (and it isn't a
 * tombstone), fill *info (may be NULL), call cb for each entry (cb may be
 * NULL) and return 1; else return 0. A v1 record answers for slots 0 and 1
 * (info->revision 0, no name). */
int config_store_read(uint8_t slot, cs_slot_info_t *info,
                      void (*cb)(const cs_entry_t *e, void *ctx), void *ctx);

/* Forget `slot`'s stored values and name (a tombstone record).
 * Returns GSX_OK or GSX_FLASH_ERR. */
int config_store_erase_slot(uint8_t slot);

/* Erase everything (FACTORY_RESET). Returns GSX_OK or GSX_FLASH_ERR. */
int config_store_erase(void);

/* Largest entry count one record can hold here (the builder checks the
 * same number, §10.5). */
uint32_t config_store_max_entries(void);

#ifdef __cplusplus
}
#endif

#endif
