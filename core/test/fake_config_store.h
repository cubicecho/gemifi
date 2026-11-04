/*
 * fake_config_store.h: test access to the RAM config store
 * (fake_config_store.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FAKE_CONFIG_STORE_H
#define FAKE_CONFIG_STORE_H

#include <stdint.h>

extern int fake_cs_writes;
extern int fake_cs_fail;
extern int fake_cs_reads;            /* config_store_read calls */

/* Entries in slot's newest record, or -1 if it has none (or a tombstone). */
int fake_cs_count(uint8_t slot);
/* 1 and *value if slot's record holds (id, index). */
int fake_cs_get(uint8_t slot, uint16_t id, uint16_t index, int32_t *value);
const char *fake_cs_name(uint8_t slot);
uint16_t fake_cs_revision(uint8_t slot);

#endif
