/* host-sim: firmware/nuvoton-nuc1xx/usb-log/main.c built natively with stubbed
 * clocks, flash UID and USB; replies are captured as raw SysEx bytes. */
#include <string.h>
#include <stdint.h>
#define main app_main
#include "main.c"
#undef main
volatile gemifi_mailbox_t sim_mailbox;
uint32_t sim_buttons;
static uint32_t ms;
static uint8_t out[4096]; static int out_n;
void sys_unlock(void) {}
void sys_clock_init(void) {}
void sys_tick_init(void) {}
uint32_t sys_tick_ms(void) { return ms += 1; }
void sys_delay_ms(uint32_t d) { ms += d; }
void sys_reset_to(int x) { for(;;); }
void fmc_open(void) {}
int fmc_read_uid(uint32_t w[3]) { uint8_t *u = (uint8_t *)w; for (int i = 0; i < 12; i++) u[i] = 0xA0 + i; return 0; }
void usbd_init(const usbd_desc_t *d) {}
void usbd_poll(void) {}
int usbd_configured(void) { return 1; }
void usbd_disconnect(void) {}
int midi_rx_poll(gsx_rx_t *r) { return 0; }
void midi_tx_event(uint8_t c, uint8_t a, uint8_t b, uint8_t d) { out[out_n++]=c; out[out_n++]=a; out[out_n++]=b; out[out_n++]=d; }
void midi_tx_sysex(const uint8_t *p, uint32_t n) { memcpy(out + out_n, p, n); out_n += n; }
int midi_tx_flush(uint32_t t) { return 0; }
void sim_init(void) { rx.buf = rx_buf; rx.cap = sizeof rx_buf; make_serial(); }
int sim_feed(const uint8_t *p, int n, uint8_t *buf) {
    out_n = 0;
    for (int i = 0; i < n; i++)
        if (gsx_rx_byte(&rx, p[i])) { handle_frame(); gsx_rx_reset(&rx); }
    memcpy(buf, out, out_n); return out_n;
}
int sim_press(int idx, int down, uint8_t *buf) { out_n = 0; button_changed(idx, down); memcpy(buf, out, out_n); return out_n; }
int sim_reboot_mode(void) { return reboot_mode; }
