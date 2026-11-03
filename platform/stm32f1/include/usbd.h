/*
 * usbd.h: minimal polled USB full-speed device core for the STM32F1 USB peripheral
 *
 * Endpoints (RM0008 ch. 23; each EPnR has its own RX and TX half):
 *   EP0R: EP0 control, IN and OUT, 64 B
 *   EP1R: EA = 1, bulk, IN half = 0x81 (64 B), OUT half = 0x01 (64 B)
 * Packet memory (512 B, local offsets): BTABLE 0x000, EP0 TX 0x040,
 * EP0 RX 0x080, 0x81 at 0x0C0, 0x01 at 0x100 (320 B used). The CPU sees
 * PMA as 16-bit words at 32-bit stride, so OUT packets are copied to RAM.
 *
 * Polled: call usbd_poll() often (every few ms at most; the host retries
 * NAKed transfers, and control transfers time out after 5 s).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_USBD_H
#define GEMIFI_USBD_H

#include <stdint.h>

typedef struct {
    const uint8_t *device;          /* 18-byte device descriptor        */
    const uint8_t *config;          /* full configuration descriptor    */
    uint16_t config_len;
    uint8_t num_strings;            /* string indices 1..num_strings    */
    const char *const *strings;     /* ASCII, <= 40 chars; [0] = index 1 */
} usbd_desc_t;

/* Reset the USB block, drive SE0 on PA11/PA12 for ~10 ms (so the host sees
 * a disconnect even if an earlier stage was enumerated), then attach. The
 * USB clock (48 MHz) must already be set up by sys_clock_init().
 * Needs sys_tick_init() first (it uses sys_delay_ms). */
void usbd_init(const usbd_desc_t *d);
void usbd_poll(void);
/* Serve USB from its interrupt (USB_LP) at NVIC priority prio (lower =
 * more urgent) instead of from usbd_poll(), which then does nothing. The
 * other calls stay safe from the main loop: they mask the interrupt
 * while they touch the endpoints. Undone by usbd_disconnect(). */
void usbd_irq_enable(uint8_t prio);
int usbd_configured(void);

/* Detach: USB block reset and clocked off, PA11/PA12 driven low (SE0),
 * the board pull-up GPIO (BOARD_USB_PULLUP_PIN) off if there is one.
 * Stays detached until usbd_init() runs again. */
void usbd_disconnect(void);

/* bulk IN 0x81 */
int usbd_in_busy(void);
void usbd_in_write(const uint8_t *p, uint32_t n);   /* n <= 64 */
void usbd_in_cancel(void);

/* bulk OUT 0x01: the packet stays in a RAM copy (and the endpoint NAKs)
 * until usbd_out_release(). */
int usbd_out_len(void);                             /* -1 = none */
const volatile uint8_t *usbd_out_data(void);
void usbd_out_release(void);

#endif
