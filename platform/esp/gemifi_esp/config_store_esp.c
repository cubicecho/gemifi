/*
 * config_store_esp.c: stored settings per slot (config_store.h) on the
 * ESP32 / S2 / S3, in NVS.
 *
 * Every record is the same v2 words as a NUC journal record (seq is 0
 * here: there is never more than one record per slot).
 *
 * One NVS blob per slot (namespace "gemifi", keys "s0".."s255": the blobs
 * the Arduino builds' Preferences wrote, so an update keeps them). NVS
 * writes are atomic, so a power cut keeps the old record. Resetting a slot
 * removes its blob. An old v1 "profile" blob answers for slots 0 and 1
 * until the first write: that write first copies it to whichever of
 * "s0"/"s1" it isn't writing, writes its own slot, then removes "profile".
 * NVS lives outside the app slots, so OTA updates keep it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gm_esp.h"
#include "nvs.h"
#include "config_store.h"
#include "gsx.h"

#define V2_HDR     4u
#define NAME_WORDS (CS_NAME_LEN / 4u)
#define NS         "gemifi"
#define REC_MAX    1984u                    /* bytes per blob (§10.5) */
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

static void key_of(char *k, uint8_t slot)
{
    snprintf(k, 6, "s%u", (unsigned)slot);
}

static int has_key(nvs_handle_t h, const char *key)
{
    size_t n = 0;
    return nvs_get_blob(h, key, NULL, &n) == ESP_OK;
}

/* A valid record from blob `key` (malloc'd; NULL if none). */
static uint32_t *blob_get(nvs_handle_t h, const char *key)
{
    size_t bytes = 0;
    if (nvs_get_blob(h, key, NULL, &bytes) != ESP_OK || bytes < 16 || bytes % 4)
        return NULL;
    uint32_t *w = (uint32_t *)malloc(bytes);
    size_t got = bytes;
    if (w && (nvs_get_blob(h, key, w, &got) != ESP_OK || got != bytes ||
              rec_words(w, bytes / 4) != bytes / 4)) {
        free(w);
        w = NULL;
    }
    return w;
}

static int blob_put(nvs_handle_t h, uint8_t slot, const uint32_t *w, uint32_t words)
{
    char k[6];
    key_of(k, slot);
    return nvs_set_blob(h, k, w, 4u * words) == ESP_OK ? GSX_OK : GSX_FLASH_ERR;
}

/* Before changing `slot`: copy a v1 "profile" to the other of slots 0/1
 * unless that has its own blob. The caller removes "profile" after. */
static int migrate(nvs_handle_t h, uint8_t slot)
{
    uint32_t *v1 = blob_get(h, "profile");
    int st = GSX_OK;
    if (v1 && is_v1(v1)) {
        uint32_t *w = (uint32_t *)malloc(4u * (V2_HDR + 1u) + 8u * v1[2]);
        for (uint8_t s = 0; s < 2 && st == GSX_OK; s++) {
            char k[6];
            key_of(k, s);
            if (s == slot || has_key(h, k))
                continue;
            st = w ? blob_put(h, s, w, from_v1(w, s, v1)) : GSX_FLASH_ERR;
        }
        free(w);
    }
    free(v1);
    return st;
}

/* Close h, committing a writer's changes first: st, or the commit's error. */
static int done(nvs_handle_t h, int st)
{
    if (st == GSX_OK && nvs_commit(h) != ESP_OK)
        st = GSX_FLASH_ERR;
    nvs_close(h);
    return st;
}

int config_store_write(uint8_t slot, uint16_t revision, const char *name,
                       const cs_entry_t *e, uint32_t n)
{
    if (n > MAX_ENTRIES)
        return GSX_BAD_LEN;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return GSX_FLASH_ERR;
    int st = migrate(h, slot);
    uint32_t *w = (uint32_t *)malloc(rec_bytes(n, name));
    if (!w)
        st = GSX_FLASH_ERR;
    if (st == GSX_OK)
        st = blob_put(h, slot, w, build(w, slot, revision, name, e, NULL, n));
    if (st == GSX_OK && has_key(h, "profile"))
        nvs_erase_key(h, "profile");
    free(w);
    return done(h, st);
}

int config_store_read(uint8_t slot, cs_slot_info_t *info,
                      void (*cb)(const cs_entry_t *e, void *ctx), void *ctx)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
        return 0;                           /* nothing stored yet: no namespace */
    char k[6];
    key_of(k, slot);
    uint32_t *w = blob_get(h, k);
    if (!w && slot <= 1u) {
        w = blob_get(h, "profile");
        if (w && !is_v1(w)) {
            free(w);
            w = NULL;
        }
    }
    int found = w ? answer(w, info, cb, ctx) : 0;
    free(w);
    nvs_close(h);
    return found;
}

int config_store_erase_slot(uint8_t slot)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return GSX_FLASH_ERR;
    int st = migrate(h, slot);
    char k[6];
    key_of(k, slot);
    if (st == GSX_OK) {
        if (has_key(h, k))
            nvs_erase_key(h, k);
        if (has_key(h, "profile"))
            nvs_erase_key(h, "profile");
    }
    return done(h, st);
}

int config_store_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return GSX_FLASH_ERR;
    /* every slot and "profile": the namespace is only ours */
    return done(h, nvs_erase_all(h) == ESP_OK ? GSX_OK : GSX_FLASH_ERR);
}

uint32_t config_store_max_entries(void)
{
    return MAX_ENTRIES;                     /* 243 */
}
