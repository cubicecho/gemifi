/*
 * fw_esp.cpp: firmware upload in the app (sysex.md 4.1 "Who takes
 * uploads", 4.2 A/B rules) through the Arduino Update class.
 *
 * The image is the plain Arduino app .bin (no .gfw header), written to the
 * inactive OTA slot in order from offset 0. FW_END checks the CRC-32 of the
 * whole file and lets Update mark the new slot to boot (ESP32: image check +
 * esp_ota_set_boot_partition; ESP8266: eboot copies it at the next boot);
 * the old slot stays bootable until then. REBOOT(0) starts it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_esp.h"
#include "gm_hal.h"
#ifdef ESP32
#include <Update.h>
#else
#include <Updater.h>
#endif
extern "C" {
#include "gsx.h"
}

static struct {
    uint8_t active;
    uint32_t size, crc, next, run;
} fw;

static void fw_abort(void)
{
    if (!fw.active)
        return;
    fw.active = 0;
#ifdef ESP32
    Update.abort();
#else
    Update.end(false);                      /* not finished: just resets */
#endif
}

static int fw_begin(uint32_t size, uint32_t crc32, uint16_t board_id, uint32_t fw_version)
{
    (void)fw_version;
    fw_abort();
    if (board_id != GM_BOARD_ID)
        return GSX_BAD_BOARD;
    if (size == 0 || (size & 3u))
        return GSX_BAD_ADDR;
    if (!Update.begin(size, U_FLASH)) {
        uint8_t e = Update.getError();
        return e == UPDATE_ERROR_SPACE || e == UPDATE_ERROR_SIZE ? GSX_BAD_ADDR
                                                                 : GSX_FLASH_ERR;
    }
    fw.active = 1;
    fw.size = size;
    fw.crc = crc32;
    fw.next = 0;
    fw.run = 0;
    return GSX_OK;
}

static int fw_data(uint32_t offset, const uint8_t *p, uint32_t n)
{
    if (!fw.active)
        return GSX_BAD_STATE;
    if (offset != fw.next || n == 0 || (n & 3u) || n > GM_ESP_MAX_CHUNK ||
        offset + n > fw.size)
        return GSX_BAD_ADDR;
    /* Update buffers a flash sector and erases + writes it when full. */
    if (Update.write((uint8_t *)p, n) != n) {
        int st = Update.getError() == UPDATE_ERROR_MAGIC_BYTE ? GSX_BAD_IMAGE
                                                              : GSX_FLASH_ERR;
        fw_abort();
        return st;
    }
    fw.run = gm_crc32(fw.run, p, n);
    fw.next += n;
    return GSX_OK;
}

static int fw_end(void)
{
    if (!fw.active)
        return GSX_BAD_STATE;
    if (fw.next != fw.size || fw.run != fw.crc) {
        fw_abort();
        return GSX_BAD_IMAGE;
    }
    fw.active = 0;
    return Update.end() ? GSX_OK : GSX_BAD_IMAGE;
}

static const hal_fw_ops_t ops = {
    GM_ESP_MAX_CHUNK, fw_begin, fw_data, fw_end, fw_abort,
};

const hal_fw_ops_t *hal_fw_ops(void)
{
    return &ops;
}

/* CRC-32/ISO-HDLC (zlib), nibble table. */
uint32_t gm_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
    static const uint32_t t[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
        0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ t[crc & 15];
        crc = (crc >> 4) ^ t[crc & 15];
    }
    return ~crc;
}
