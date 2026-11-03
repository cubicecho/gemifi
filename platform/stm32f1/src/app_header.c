/*
 * app_header.c: placeholder .gfw header for apps. The linker puts it at
 * app offset 0x130 = 0x08002530 (ld/app.ld); tools/host/mkimage.py then fills
 * in image_size and crc32
 * (and rewrites board/version from its command line).
 *
 * FW_VERSION_MAJOR/MINOR/PATCH come from the app Makefile.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gemifi_image.h"
#include "gsx.h"
#include "board.h"

#ifndef FW_VERSION_MAJOR
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 0
#define FW_VERSION_PATCH 0
#endif

__attribute__((section(".gemifi_hdr"), used))
const struct gemifi_img_hdr gemifi_app_header = {
    .magic = GEMIFI_IMG_MAGIC,
    .hdr_version = GEMIFI_IMG_HDR_VERSION,
    .board_id = BOARD_ID,
    .image_size = 0,                 /* mkimage */
    .crc32 = 0,                      /* mkimage */
    .fw_version = GEMIFI_VERSION(FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH),
    .proto_version = GSX_PROTO_VERSION,
    .reserved = { 0xFFFFFFFFu, 0xFFFFFFFFu },
};
