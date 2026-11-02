/*
 * fake_config_store.c: config_store.h in RAM for the core host tests. It
 * keeps the newest record of every slot, the way the real stores answer.
 * The journals themselves are tested per platform
 * (platform/nuvoton-nuc1xx/test/host-sim, ...).
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "config_store.h"
#include "gsx.h"
#include "fake_config_store.h"

#define FAKE_SLOTS   65
#define FAKE_ENTRIES 256

typedef struct {
    int used;                        /* a record exists (tombstone: 0) */
    uint16_t revision;
    uint8_t flags;
    char name[CS_NAME_LEN + 1];
    uint32_t n;
    cs_entry_t e[FAKE_ENTRIES];
} fake_rec_t;

static fake_rec_t recs[FAKE_SLOTS];
int fake_cs_writes;                  /* records written (incl. tombstones) */
int fake_cs_fail;                    /* next writes fail with GSX_FLASH_ERR */

int config_store_write(uint8_t slot, uint16_t revision, const char *name,
                       const cs_entry_t *e, uint32_t n)
{
    if (fake_cs_fail)
        return GSX_FLASH_ERR;
    if (slot >= FAKE_SLOTS || n > FAKE_ENTRIES)
        return GSX_BAD_LEN;
    fake_rec_t *r = &recs[slot];
    r->used = 1;
    r->revision = revision;
    r->flags = name ? CS_F_NAME : 0;
    memset(r->name, 0, sizeof r->name);
    if (name)
        strncpy(r->name, name, CS_NAME_LEN);
    r->n = n;
    if (n)
        memcpy(r->e, e, n * sizeof *e);
    fake_cs_writes++;
    return GSX_OK;
}

int config_store_read(uint8_t slot, cs_slot_info_t *info,
                      void (*cb)(const cs_entry_t *e, void *ctx), void *ctx)
{
    if (slot >= FAKE_SLOTS || !recs[slot].used)
        return 0;
    const fake_rec_t *r = &recs[slot];
    if (info) {
        info->revision = r->revision;
        info->flags = r->flags;
        memcpy(info->name, r->name, sizeof info->name);
    }
    if (cb)
        for (uint32_t i = 0; i < r->n; i++)
            cb(&r->e[i], ctx);
    return 1;
}

int config_store_erase_slot(uint8_t slot)
{
    if (fake_cs_fail)
        return GSX_FLASH_ERR;
    if (slot < FAKE_SLOTS)
        recs[slot].used = 0;
    fake_cs_writes++;
    return GSX_OK;
}

int config_store_erase(void)
{
    if (fake_cs_fail)
        return GSX_FLASH_ERR;
    memset(recs, 0, sizeof recs);
    return GSX_OK;
}

uint32_t config_store_max_entries(void)
{
    return FAKE_ENTRIES;
}

/* ---- test access -------------------------------------------------------- */

int fake_cs_count(uint8_t slot)
{
    return slot < FAKE_SLOTS && recs[slot].used ? (int)recs[slot].n : -1;
}

int fake_cs_get(uint8_t slot, uint16_t id, uint16_t index, int32_t *value)
{
    if (slot >= FAKE_SLOTS || !recs[slot].used)
        return 0;
    for (uint32_t i = 0; i < recs[slot].n; i++)
        if (recs[slot].e[i].id == id && recs[slot].e[i].index == index) {
            if (value)
                *value = recs[slot].e[i].value;
            return 1;
        }
    return 0;
}

const char *fake_cs_name(uint8_t slot)
{
    return slot < FAKE_SLOTS && recs[slot].used ? recs[slot].name : 0;
}

uint16_t fake_cs_revision(uint8_t slot)
{
    return slot < FAKE_SLOTS ? recs[slot].revision : 0;
}
