/*
 * fw_esp.c: firmware upload in the app (sysex.md 4.1 "Who takes uploads",
 * 4.2 A/B rules) through ESP-IDF's OTA API.
 *
 * The image is the plain ESP-IDF app .bin (no .gfw header), written to the
 * inactive OTA slot in order from offset 0. Each flash sector is erased as
 * the writes reach it (OTA_WITH_SEQUENTIAL_WRITES), so FW_BEGIN answers at
 * once. FW_END checks the CRC-32 of the whole file, has ESP-IDF check the
 * image (its header, segments and SHA-256) and marks the new slot to boot;
 * the old slot stays bootable until then. REBOOT(0) starts it.
 *
 * Rollback (sdkconfig CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): the bootloader
 * starts the new app as "pending verify"; it is kept once it confirms
 * itself (gm_esp_app_confirm), and a reset before that (a crash, the
 * watchdog, a power cut) boots the previous app again. ESP-IDF won't open
 * the other slot for an update while the running app is pending (it holds
 * the only app known to work), so FW_BEGIN confirms first: a host reaching
 * the app over MIDI is proof enough.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_esp.h"
#include "gm_hal.h"
#include "gsx.h"
#include "esp_ota_ops.h"

static struct {
    uint8_t active;
    uint32_t size, crc, next, run;
    const esp_partition_t *part;
    esp_ota_handle_t h;
} fw;

static void fw_abort(void)
{
    if (!fw.active)
        return;
    fw.active = 0;
    esp_ota_abort(fw.h);
}

void gm_esp_app_confirm(void)
{
    static uint8_t done;
    esp_ota_img_states_t st;
    if (done)
        return;
    done = 1;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY)
        esp_ota_mark_app_valid_cancel_rollback();
}

static int fw_begin(uint32_t size, uint32_t crc32, uint16_t board_id, uint32_t fw_version)
{
    (void)fw_version;
    fw_abort();
    gm_esp_app_confirm();
    if (board_id != GM_BOARD_ID)
        return GSX_BAD_BOARD;
    if (size == 0 || (size & 3u))
        return GSX_BAD_ADDR;
    fw.part = esp_ota_get_next_update_partition(NULL);
    if (!fw.part || size > fw.part->size)
        return GSX_BAD_ADDR;
    if (esp_ota_begin(fw.part, OTA_WITH_SEQUENTIAL_WRITES, &fw.h) != ESP_OK)
        return GSX_FLASH_ERR;
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
    if (offset == 0 && p[0] != 0xE9) {      /* ESP_IMAGE_HEADER_MAGIC: not an app image */
        fw_abort();
        return GSX_BAD_IMAGE;
    }
    esp_err_t e = esp_ota_write(fw.h, p, n);
    if (e != ESP_OK) {
        fw_abort();
        return e == ESP_ERR_OTA_VALIDATE_FAILED ? GSX_BAD_IMAGE : GSX_FLASH_ERR;
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
    if (esp_ota_end(fw.h) != ESP_OK)        /* the image check; the handle is freed either way */
        return GSX_BAD_IMAGE;
    return esp_ota_set_boot_partition(fw.part) == ESP_OK ? GSX_OK : GSX_FLASH_ERR;
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
