/*
 * gemifi_image.h: the .gfw image header (protocol/sysex.md 5), STM32F1
 * flavour. Same 32-byte struct as the NUC1xx, but the app starts at
 * GEMIFI_IMG_BASE (BOARD_APP_BASE from the board profile, above the
 * bootloader: 0x08002400 on the Code 61) and the header sits after the
 * 0x130-byte vector table (16 core + 60 IRQ entries), at app offset 0x130.
 * image_size and crc32 cover [GEMIFI_IMG_BASE, GEMIFI_IMG_BASE + image_size).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_IMAGE_H
#define GEMIFI_IMAGE_H

#include <stdint.h>
#include "board_layout.h"

#define GEMIFI_IMG_MAGIC       0x49464D47u   /* 'GMFI' */
#define GEMIFI_IMG_HDR_VERSION 1
#define GEMIFI_IMG_BASE        BOARD_APP_BASE   /* app link address */
#define GEMIFI_IMG_HDR_OFFSET  0x130u        /* from GEMIFI_IMG_BASE */
#define GEMIFI_IMG_CRC_OFFSET  0x13Cu        /* crc32 field, taken as 0 */
#define GEMIFI_IMG_HDR_END     0x150u
/* App area limit (BOARD_APP_END). Code 61: the page at 0x0801B000 belongs to
 * the vendor update packer (whole-image checksum at 0x0801B3FE), so the
 * image ends below it. */
#define GEMIFI_IMG_LIMIT       BOARD_APP_END

/* The gemifi bootloader (bootloader/stm32f1) at BOARD_FLASH_BASE marks itself
 * with this right after its vector table, so an app can tell it from
 * M-Audio's bootloader and say so in HELLO (caps bit 7, protocol/sysex.md 4.1). */
#define GEMIFI_BOOT_MARK_ADDR  (BOARD_FLASH_BASE + 0x130u)
#define GEMIFI_BOOT_MAGIC      0x4C424D47u   /* 'GMBL' */

struct gemifi_boot_mark {
    uint32_t magic;
    uint16_t board_id;
    uint16_t mark_version;     /* 1 */
    uint32_t boot_version;     /* major << 24 | minor << 16 | patch */
    uint32_t reserved;
};

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
