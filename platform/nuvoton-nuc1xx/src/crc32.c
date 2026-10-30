/*
 * crc32.c: reflected CRC-32, polynomial 0xEDB88320, 16-entry table (64 B).
 *
 * SPDX-License-Identifier: MIT
 */
#include "crc32.h"

static const uint32_t crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

static uint32_t crc32_byte(uint32_t crc, uint32_t b)
{
    crc ^= b;
    crc = (crc >> 4) ^ crc_nibble[crc & 0xFu];
    crc = (crc >> 4) ^ crc_nibble[crc & 0xFu];
    return crc;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    while (n--)
        crc = crc32_byte(crc, *p++);
    return crc;
}

uint32_t crc32_word(uint32_t crc, uint32_t w)
{
    for (int i = 0; i < 4; i++) {
        crc = crc32_byte(crc, w & 0xFFu);
        w >>= 8;
    }
    return crc;
}
