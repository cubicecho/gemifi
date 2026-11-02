/*
 * crc32.h: CRC-32/ISO-HDLC (zlib crc32), nibble-table implementation.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_CRC32_H
#define GEMIFI_CRC32_H

#include <stdint.h>

#define CRC32_INIT 0xFFFFFFFFu

/* Running form: crc = CRC32_INIT; crc = crc32_update(crc, ...); final = ~crc */
uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);
uint32_t crc32_word(uint32_t crc, uint32_t word);   /* 4 bytes, LE */

#endif
