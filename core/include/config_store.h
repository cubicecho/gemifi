/*
 * config_store.h: the stored profile (sysex.md 4.3 COMMIT). Implemented per
 * platform: NUC1xx data flash (platform/nuvoton-nuc1xx/src/config_store.c,
 * described below), ESP32 NVS blob / ESP8266 EEPROM sector
 * (platform/esp/config_store_esp.cpp, same record words).
 *
 * NUC1xx: data flash (4 KB at 0x1F000) minus its last page (the device ID,
 * hal.c), so 7 pages of 512 B, is used as a journal of
 * records. Each COMMIT appends one record after the newest; when a page is
 * full the next page (wrapping) is erased and used. The newest record is
 * never erased before a newer one is written, so a power cut during COMMIT
 * keeps the previous profile. Record, in 32-bit words:
 *
 *   magic 'GCFG' (0x47464347), seq, count,
 *   count x { id | index << 16, value },
 *   crc32 (zlib) over all the words before it
 *
 * Records never span a page. The newest valid record (highest seq) wins.
 * Entries are tagged by param id, so a profile survives firmware updates
 * that add or remove params.
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

#define CONFIG_STORE_MAGIC 0x47464347u   /* 'GCFG' */

/* Apply the newest stored record on top of the current values (call
 * params_defaults() first). Returns 1 if a record was found, 0 if not. */
int config_store_load(void);

/* Append the current non-VOLATILE, non-READ_ONLY values.
 * Returns GSX_OK or GSX_FLASH_ERR. */
int config_store_commit(void);

/* Erase every page. Returns GSX_OK or GSX_FLASH_ERR. */
int config_store_erase(void);

#ifdef __cplusplus
}
#endif

#endif
