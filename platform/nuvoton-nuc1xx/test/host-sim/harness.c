/* host-sim: the real bootloader main.c, built natively against a simulated flash,
 * mailbox and button state. USB is stubbed: frames go straight to handle_frame(). */
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#define main boot_main
#include "main.c"
#undef main

volatile gemifi_mailbox_t sim_mailbox;
uint32_t sim_buttons;
static uint32_t ms;
static jmp_buf jb;
int sim_result;              /* 1 = handoff app (BS=0), 2 = reset to LDROM, 3 = update mode */
int sim_bad_writes;          /* writes/erases outside the allowed region */
uint8_t sim_flash[0x20000];  /* APROM 0..0x8000, data flash 0x1F000 */
static uint8_t out[4096]; static int out_n;

void sys_unlock(void) {}
void sys_clock_init(void) {}
void sys_tick_init(void) {}
uint32_t sys_tick_ms(void) { return ms += 1; }
void sys_delay_ms(uint32_t d) { ms += d; }
void sys_reset_to(int to_ldrom) { longjmp(jb, to_ldrom ? 2 : 1); }

static int allowed(uint32_t a) {
#ifdef TEST_IN_APROM
    return a >= 0x1F000 && a < 0x20000;
#else
    return a < 0x8000;
#endif
}
void fmc_open(void) {}
uint32_t fmc_read(uint32_t a) { uint32_t w; if (a + 4 > sizeof sim_flash) return 0xFFFFFFFF; memcpy(&w, sim_flash + a, 4); return w; }
int fmc_erase_page(uint32_t a) { if (!allowed(a) || (a & 511)) { sim_bad_writes++; return -1; } memset(sim_flash + a, 0xFF, 512); return 0; }
int fmc_program(uint32_t a, uint32_t w) { if (!allowed(a) || (a & 3)) { sim_bad_writes++; return -1; } uint32_t o; memcpy(&o, sim_flash + a, 4); o &= w; memcpy(sim_flash + a, &o, 4); return 0; }
int fmc_read_uid(uint8_t u[12]) { for (int i = 0; i < 12; i++) u[i] = 0xA0 + i; return 0; }

void usbd_init(const usbd_desc_t *d) { (void)d; longjmp(jb, 3); }
void usbd_poll(void) {}
int usbd_configured(void) { return 1; }
void usbd_disconnect(void) {}
int midi_rx_poll(gsx_rx_t *r) { (void)r; return 0; }
void midi_tx_sysex(const uint8_t *p, uint32_t n) { memcpy(out + out_n, p, n); out_n += n; }
int midi_tx_flush(uint32_t t) { (void)t; return 0; }

/* run the reset path; returns sim_result */
int sim_boot(void) {
    int r = setjmp(jb);
    if (r) { sim_result = r; return r; }
    boot_main();
    return -1;
}
/* make the update-mode state usable without the endless loop */
void sim_enter_update(void) { make_serial(); rx.buf = rx_buf; rx.cap = sizeof rx_buf; }

/* feed raw MIDI bytes; returns number of reply bytes in buf */
int sim_feed(const uint8_t *p, int n, uint8_t *buf) {
    out_n = 0;
    for (int i = 0; i < n; i++)
        if (gsx_rx_byte(&rx, p[i])) { handle_frame(); gsx_rx_reset(&rx); }
    memcpy(buf, out, out_n);
    return out_n;
}
int sim_reboot_mode(void) { return reboot_mode; }
const char *sim_serial(void) { return serial; }
