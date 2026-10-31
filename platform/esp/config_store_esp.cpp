/*
 * config_store_esp.cpp: the stored profile (config_store.h) on ESP.
 *
 * One record, the same words as the NUC journal record:
 *   magic 'GCFG', seq (0), count, count x { id | index << 16, value }, crc32
 * ESP32: an NVS blob (Preferences "gemifi" / "profile"; NVS writes are
 * atomic, so a power cut keeps the old profile). ESP8266: the EEPROM sector
 * (erase + write: a power cut during COMMIT loses the profile, the CRC then
 * fails and the defaults are used). Both live outside the app slots, so
 * OTA updates keep the profile; unknown ids are skipped on load.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_esp.h"
#ifdef ESP32
#include <Preferences.h>
#else
#include <EEPROM.h>
#endif
extern "C" {
#include "config_store.h"
#include "gsx.h"
#include "params.h"
}

#define EE_SIZE 4096u                       /* ESP8266: one flash sector */
#define SKIP (PF_VOLATILE | PF_READ_ONLY)

static uint32_t stored_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < app_params.param_count; i++)
        if (!(app_params.params[i].flags & SKIP))
            n += app_params.params[i].count;
    return n;
}

static int blob_write(const uint32_t *w, uint32_t bytes)
{
#ifdef ESP32
    Preferences p;
    if (!p.begin("gemifi", false))
        return GSX_FLASH_ERR;
    size_t n = p.putBytes("profile", w, bytes);
    p.end();
    return n == bytes ? GSX_OK : GSX_FLASH_ERR;
#else
    if (bytes > EE_SIZE)
        return GSX_FLASH_ERR;
    EEPROM.begin(EE_SIZE);
    memcpy(EEPROM.getDataPtr(), w, bytes);
    int ok = EEPROM.commit();
    EEPROM.end();
    return ok ? GSX_OK : GSX_FLASH_ERR;
#endif
}

int config_store_commit(void)
{
    uint32_t n = stored_count();
    uint32_t words = 3 + 2 * n + 1;
    uint32_t *w = (uint32_t *)malloc(words * 4);
    if (!w)
        return GSX_FLASH_ERR;
    uint32_t k = 0;
    w[k++] = CONFIG_STORE_MAGIC;
    w[k++] = 0;
    w[k++] = n;
    for (uint32_t i = 0; i < app_params.param_count; i++) {
        const param_def_t *d = &app_params.params[i];
        if (d->flags & SKIP)
            continue;
        for (uint32_t j = 0; j < d->count; j++) {
            w[k++] = d->id | j << 16;
            w[k++] = (uint32_t)d->values[j];
        }
    }
    w[k] = gm_crc32(0, (const uint8_t *)w, k * 4);
    int st = blob_write(w, words * 4);
    free(w);
    return st;
}

/* Checks a record and applies its entries. */
static int apply(const uint32_t *w, uint32_t bytes)
{
    if (bytes < 16 || w[0] != CONFIG_STORE_MAGIC)
        return 0;
    uint32_t n = w[2];
    if (n > (bytes - 16) / 8)
        return 0;
    uint32_t k = 3 + 2 * n;
    if (gm_crc32(0, (const uint8_t *)w, k * 4) != w[k])
        return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t id = (uint16_t)(w[3 + 2 * i] & 0xFFFF);
        uint16_t index = (uint16_t)(w[3 + 2 * i] >> 16);
        const param_def_t *d = params_find(id);
        if (d && !(d->flags & SKIP))        /* unknown ids: ignored */
            params_set(id, index, (int32_t)w[4 + 2 * i], 0);
    }
    return 1;
}

int config_store_load(void)
{
    int found = 0;
#ifdef ESP32
    Preferences p;
    if (!p.begin("gemifi", true))
        return 0;
    size_t bytes = p.getBytesLength("profile");
    uint32_t *w = bytes ? (uint32_t *)malloc(bytes) : NULL;
    if (w && p.getBytes("profile", w, bytes) == bytes)
        found = apply(w, bytes);
    free(w);
    p.end();
#else
    EEPROM.begin(EE_SIZE);
    found = apply((const uint32_t *)EEPROM.getConstDataPtr(), EE_SIZE);
    EEPROM.end();
#endif
    return found;
}

int config_store_erase(void)
{
#ifdef ESP32
    Preferences p;
    if (!p.begin("gemifi", false))
        return GSX_FLASH_ERR;
    p.remove("profile");                    /* false if absent: fine */
    p.end();
    return GSX_OK;
#else
    uint32_t blank = 0xFFFFFFFFu;
    return blob_write(&blank, 4);
#endif
}
