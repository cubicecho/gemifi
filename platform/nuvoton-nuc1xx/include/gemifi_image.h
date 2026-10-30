/*
 * gemifi_image.h: the .gfw image header at 0x0C0 (protocol/sysex.md 5).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_IMAGE_H
#define GEMIFI_IMAGE_H

#include <stdint.h>

#define GEMIFI_IMG_MAGIC       0x49464D47u   /* 'GMFI' */
#define GEMIFI_IMG_HDR_VERSION 1
#define GEMIFI_IMG_HDR_OFFSET  0x0C0u
#define GEMIFI_IMG_CRC_OFFSET  0x0CCu        /* crc32 field, taken as 0 */
#define GEMIFI_IMG_HDR_END     0x0E0u

struct gemifi_img_hdr {
    uint32_t magic;
    uint16_t hdr_version;
    uint16_t board_id;
    uint32_t image_size;
    uint32_t crc32;
    uint32_t fw_version;       /* major << 24 | minor << 16 | patch */
    uint32_t proto_version;
    uint32_t reserved[2];
};

_Static_assert(sizeof(struct gemifi_img_hdr) == 32, "header is 32 bytes");

#define GEMIFI_VERSION(maj, min, pat) \
    (((uint32_t)(maj) << 24) | ((uint32_t)(min) << 16) | (uint32_t)(pat))

#endif
