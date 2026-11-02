/*
 * config_store_esp.cpp: stored settings per slot (config_store.h) on ESP.
 *
 * Every record is the same v2 words as a NUC journal record (seq is 0
 * here: there is never more than one record per slot).
 *
 * ESP32: one NVS blob per slot (Preferences "gemifi", keys "s0".."s64").
 * NVS writes are atomic, so a power cut keeps the old record. Resetting a
 * slot removes its blob. An old v1 "profile" blob answers for slots 0 and
 * 1 until the first write: that write first copies it to whichever of
 * "s0"/"s1" it isn't writing, writes its own slot, then removes "profile".
 *
 * ESP8266: the 4 KB EEPROM sector holds every slot's record, packed from
 * offset 0 (the first word that doesn't start a valid record ends the
 * list). Each write rewrites the whole sector: a power cut during COMMIT
 * can lose all of them (the CRCs then fail and the defaults are used). An
 * old v1 image (one 'GCFG' record) answers for slots 0 and 1; the first
 * write turns it into one v2 record per slot.
 *
 * Both live outside the app slots, so OTA updates keep them. Needs
 * arduino-esp32 >= 2.0 (Preferences::isKey).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_esp.h"
#ifdef ESP32
#include <Preferences.h>
#else
#include <EEPROM.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
#include "config_store.h"
#include "gsx.h"
}

#define V2_HDR     4u
#define NAME_WORDS (CS_NAME_LEN / 4u)
#ifdef ESP32
#define NS         "gemifi"
#define REC_MAX    1984u                    /* bytes per blob (§10.5) */
#else
#define EE_SIZE    4096u                    /* one flash sector, all slots */
#define REC_MAX    EE_SIZE
#endif
#define MAX_ENTRIES ((REC_MAX / 4u - V2_HDR - NAME_WORDS - 1u) / 2u)

/* Words of the valid record at w (at most `avail` words), else 0. */
static uint32_t rec_words(const uint32_t *w, uint32_t avail)
{
    uint32_t n, count;
    if (avail < V2_HDR)
        return 0;
    if (w[0] == CONFIG_STORE_MAGIC_V2) {
        n = V2_HDR + ((w[2] >> 8) & CS_F_NAME ? NAME_WORDS : 0u);
        count = w[3];
    } else if (w[0] == CONFIG_STORE_MAGIC) {
        n = 3;
        count = w[2];
    } else {
        return 0;
    }
    if (count > avail)
        return 0;
    n += 2u * count + 1u;
    if (n > avail || gm_crc32(0, (const uint8_t *)w, 4u * (n - 1u)) != w[n - 1u])
        return 0;
    return n;
}

static int is_v1(const uint32_t *w)
{
    return w[0] == CONFIG_STORE_MAGIC;
}

/* Answer a read from a valid record (v1: slots 0/1, revision 0, no name). */
static int answer(const uint32_t *w, cs_slot_info_t *info,
                  void (*cb)(const cs_entry_t *e, void *ctx), void *ctx)
{
    uint32_t hdr = is_v1(w) ? 0u : w[2];
    uint32_t count = is_v1(w) ? w[2] : w[3];
    const uint32_t *ent = w + (is_v1(w) ? 3u : V2_HDR);
    if ((hdr >> 8) & CS_F_TOMBSTONE)
        return 0;
    if (info) {
        memset(info->name, 0, sizeof info->name);
        info->revision = (uint16_t)(hdr >> 16);
        info->flags = (uint8_t)(hdr >> 8);
        if ((hdr >> 8) & CS_F_NAME) {
            memcpy(info->name, ent, CS_NAME_LEN);   /* little-endian words = the bytes */
            ent += NAME_WORDS;
        }
    }
    else if ((hdr >> 8) & CS_F_NAME)
        ent += NAME_WORDS;
    for (uint32_t i = 0; cb && i < count; i++) {
        cs_entry_t e = { (uint16_t)ent[2 * i], (uint16_t)(ent[2 * i] >> 16), (int32_t)ent[2 * i + 1] };
        cb(&e, ctx);
    }
    return 1;
}

/* Write a v2 record at w; entries from e (or raw words from raw). Returns
 * its words. */
static uint32_t build(uint32_t *w, uint8_t slot, uint16_t revision, const char *name,
                      const cs_entry_t *e, const uint32_t *raw, uint32_t n)
{
    uint32_t k = 0;
    w[k++] = CONFIG_STORE_MAGIC_V2;
    w[k++] = 0;
    w[k++] = (uint32_t)slot | (uint32_t)revision << 16 | (name && name[0] ? CS_F_NAME << 8 : 0u);
    w[k++] = n;
    if (name && name[0]) {
        memset(w + k, 0, CS_NAME_LEN);
        strncpy((char *)(w + k), name, CS_NAME_LEN);
        k += NAME_WORDS;
    }
    for (uint32_t i = 0; i < n; i++) {
        w[k++] = raw ? raw[2 * i] : (uint32_t)e[i].id | (uint32_t)e[i].index << 16;
        w[k++] = raw ? raw[2 * i + 1] : (uint32_t)e[i].value;
    }
    w[k] = gm_crc32(0, (const uint8_t *)w, 4u * k);
    return k + 1u;
}

/* A v1 record as a v2 record of `slot`. */
static uint32_t from_v1(uint32_t *w, uint8_t slot, const uint32_t *v1)
{
    return build(w, slot, 0, NULL, NULL, v1 + 3, v1[2]);
}

static uint32_t rec_bytes(uint32_t n, const char *name)
{
    return 4u * (V2_HDR + (name && name[0] ? NAME_WORDS : 0u) + 2u * n + 1u);
}

#ifdef ESP32 /* ---------------------------------------------------------- */

static void key_of(char *k, uint8_t slot)
{
    snprintf(k, 6, "s%u", (unsigned)slot);
}

/* A valid record from blob `key` (malloc'd; NULL if none). */
static uint32_t *blob_get(Preferences &p, const char *key)
{
    size_t bytes = p.isKey(key) ? p.getBytesLength(key) : 0;
    if (bytes < 16 || bytes % 4)
        return NULL;
    uint32_t *w = (uint32_t *)malloc(bytes);
    if (w && (p.getBytes(key, w, bytes) != bytes || rec_words(w, bytes / 4) != bytes / 4)) {
        free(w);
        w = NULL;
    }
    return w;
}

static int blob_put(Preferences &p, uint8_t slot, const uint32_t *w, uint32_t words)
{
    char k[6];
    key_of(k, slot);
    return p.putBytes(k, w, 4u * words) == 4u * words ? GSX_OK : GSX_FLASH_ERR;
}

/* Before changing `slot`: copy a v1 "profile" to the other of slots 0/1
 * unless that has its own blob. The caller removes "profile" after. */
static int migrate(Preferences &p, uint8_t slot)
{
    uint32_t *v1 = blob_get(p, "profile");
    int st = GSX_OK;
    if (v1 && is_v1(v1)) {
        uint32_t *w = (uint32_t *)malloc(4u * (V2_HDR + 1u) + 8u * v1[2]);
        for (uint8_t s = 0; s < 2 && st == GSX_OK; s++) {
            char k[6];
            key_of(k, s);
            if (s == slot || p.isKey(k))
                continue;
            st = w ? blob_put(p, s, w, from_v1(w, s, v1)) : GSX_FLASH_ERR;
        }
        free(w);
    }
    free(v1);
    return st;
}

int config_store_write(uint8_t slot, uint16_t revision, const char *name,
                       const cs_entry_t *e, uint32_t n)
{
    if (n > MAX_ENTRIES)
        return GSX_BAD_LEN;
    Preferences p;
    if (!p.begin(NS, false))
        return GSX_FLASH_ERR;
    int st = migrate(p, slot);
    uint32_t *w = (uint32_t *)malloc(rec_bytes(n, name));
    if (!w)
        st = GSX_FLASH_ERR;
    if (st == GSX_OK)
        st = blob_put(p, slot, w, build(w, slot, revision, name, e, NULL, n));
    if (st == GSX_OK && p.isKey("profile"))
        p.remove("profile");
    free(w);
    p.end();
    return st;
}

int config_store_read(uint8_t slot, cs_slot_info_t *info,
                      void (*cb)(const cs_entry_t *e, void *ctx), void *ctx)
{
    Preferences p;
    if (!p.begin(NS, true))
        return 0;
    char k[6];
    key_of(k, slot);
    uint32_t *w = blob_get(p, k);
    if (!w && slot <= 1u) {
        w = blob_get(p, "profile");
        if (w && !is_v1(w)) {
            free(w);
            w = NULL;
        }
    }
    int found = w ? answer(w, info, cb, ctx) : 0;
    free(w);
    p.end();
    return found;
}

int config_store_erase_slot(uint8_t slot)
{
    Preferences p;
    if (!p.begin(NS, false))
        return GSX_FLASH_ERR;
    int st = migrate(p, slot);
    char k[6];
    key_of(k, slot);
    if (st == GSX_OK) {
        if (p.isKey(k))
            p.remove(k);
        if (p.isKey("profile"))
            p.remove("profile");
    }
    p.end();
    return st;
}

int config_store_erase(void)
{
    Preferences p;
    if (!p.begin(NS, false))
        return GSX_FLASH_ERR;
    char k[6];
    for (unsigned s = 0; s <= 0xFFu; s++) {
        key_of(k, (uint8_t)s);
        if (p.isKey(k))
            p.remove(k);
    }
    if (p.isKey("profile"))
        p.remove("profile");
    p.end();
    return GSX_OK;
}

#else /* ESP8266 ------------------------------------------------------- */

#define EE_WORDS (EE_SIZE / 4u)

static uint32_t rec_slot(const uint32_t *w)
{
    return w[2] & 0xFFu;
}

/* The record of `slot` in the image, or NULL. */
static const uint32_t *ee_find(const uint32_t *img, uint8_t slot)
{
    uint32_t n;
    if (rec_words(img, EE_WORDS) && is_v1(img))
        return slot <= 1u ? img : NULL;
    for (uint32_t a = 0; a < EE_WORDS && (n = rec_words(img + a, EE_WORDS - a)); a += n)
        if (rec_slot(img + a) == slot)
            return img + a;
    return NULL;
}

/* Rewrite the image: every record but `slot`'s, then rec (NULL: none). */
static int ee_rewrite(uint8_t slot, const uint32_t *rec, uint32_t words)
{
    uint32_t *out = (uint32_t *)malloc(EE_SIZE);
    if (!out)
        return GSX_FLASH_ERR;
    memset(out, 0xFF, EE_SIZE);
    EEPROM.begin(EE_SIZE);
    const uint32_t *img = (const uint32_t *)EEPROM.getConstDataPtr();
    uint32_t k = 0, n;
    int st = GSX_OK;
    if (rec_words(img, EE_WORDS) && is_v1(img)) {
        for (uint8_t s = 0; s < 2; s++)
            if (s != slot && k + V2_HDR + 1u + 2u * img[2] <= EE_WORDS)
                k += from_v1(out + k, s, img);
    } else {
        for (uint32_t a = 0; a < EE_WORDS && (n = rec_words(img + a, EE_WORDS - a)); a += n)
            if (rec_slot(img + a) != slot && k + n <= EE_WORDS) {
                memcpy(out + k, img + a, 4u * n);
                k += n;
            }
    }
    if (rec && k + words > EE_WORDS)
        st = GSX_FLASH_ERR;                 /* all slots together don't fit */
    if (st == GSX_OK) {
        if (rec)
            memcpy(out + k, rec, 4u * words);
        memcpy(EEPROM.getDataPtr(), out, EE_SIZE);
        if (!EEPROM.commit())
            st = GSX_FLASH_ERR;
    }
    EEPROM.end();
    free(out);
    return st;
}

int config_store_write(uint8_t slot, uint16_t revision, const char *name,
                       const cs_entry_t *e, uint32_t n)
{
    if (n > MAX_ENTRIES)
        return GSX_BAD_LEN;
    uint32_t *w = (uint32_t *)malloc(rec_bytes(n, name));
    if (!w)
        return GSX_FLASH_ERR;
    int st = ee_rewrite(slot, w, build(w, slot, revision, name, e, NULL, n));
    free(w);
    return st;
}

int config_store_read(uint8_t slot, cs_slot_info_t *info,
                      void (*cb)(const cs_entry_t *e, void *ctx), void *ctx)
{
    EEPROM.begin(EE_SIZE);
    const uint32_t *w = ee_find((const uint32_t *)EEPROM.getConstDataPtr(), slot);
    int found = w ? answer(w, info, cb, ctx) : 0;
    EEPROM.end();
    return found;
}

int config_store_erase_slot(uint8_t slot)
{
    EEPROM.begin(EE_SIZE);
    int has = ee_find((const uint32_t *)EEPROM.getConstDataPtr(), slot) != NULL;
    EEPROM.end();
    return has ? ee_rewrite(slot, NULL, 0) : GSX_OK;
}

int config_store_erase(void)
{
    EEPROM.begin(EE_SIZE);
    memset(EEPROM.getDataPtr(), 0xFF, EE_SIZE);
    int ok = EEPROM.commit();
    EEPROM.end();
    return ok ? GSX_OK : GSX_FLASH_ERR;
}

#endif /* ---------------------------------------------------------------- */

uint32_t config_store_max_entries(void)
{
    return MAX_ENTRIES;                     /* ESP32 243, ESP8266 507 */
}
