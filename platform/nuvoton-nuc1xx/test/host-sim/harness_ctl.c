/* host-sim: the midi-controller's core (core/src incl. presets.c and
 * app_proto.c) with the generated Keystation device config and the real
 * NUC config_store.c, built natively. Driven by run_presets.py.
 *
 * Stubbed: the HAL (gm_hal.h): time (sim_ms, advanced from Python), pins
 * (buttons and the key matrix are computed from sim state on every read),
 * the ADC, MIDI out (every message is captured, length-prefixed), the UID,
 * reboot. fmc_* run on a data-flash array that the caller owns (so it
 * survives a "reboot", which is a dlclose + dlopen of this library) with
 * real erase / AND-program semantics; the caller's counters array gets
 * { erases, programs, programs onto a non-blank word, bad addresses }. */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_device_config.h"
#include "nuc1xx.h"
#include "fmc.h"

/* ---- data flash ------------------------------------------------------- */

static uint8_t *flash;           /* FLASH_DATA_SIZE bytes at FLASH_DATA_BASE */
static uint32_t *cnt;            /* erases, programs, overprograms, bad addresses */

void sim_set_flash(uint8_t *f, uint32_t *counters) { flash = f; cnt = counters; }

void fmc_open(void) {}
uint32_t fmc_read(uint32_t a)
{
    uint32_t w;
    if (a < FLASH_DATA_BASE || a + 4 > FLASH_DATA_BASE + FLASH_DATA_SIZE)
        return 0xFFFFFFFFu;
    memcpy(&w, flash + (a - FLASH_DATA_BASE), 4);
    return w;
}
int fmc_erase_page(uint32_t a)
{
    /* the device-ID page (last) is the HAL's, never the journal's */
    if (a < FLASH_DATA_BASE || a >= FLASH_DEVID_ADDR || (a & 511)) { cnt[3]++; return -1; }
    memset(flash + (a - FLASH_DATA_BASE), 0xFF, 512);
    cnt[0]++;
    return 0;
}
int fmc_program(uint32_t a, uint32_t w)
{
    if (a < FLASH_DATA_BASE || a >= FLASH_DEVID_ADDR || (a & 3)) { cnt[3]++; return -1; }
    uint32_t o;
    memcpy(&o, flash + (a - FLASH_DATA_BASE), 4);
    if (o != 0xFFFFFFFFu)
        cnt[2]++;
    o &= w;
    memcpy(flash + (a - FLASH_DATA_BASE), &o, 4);
    cnt[1]++;
    return 0;
}

/* ---- time ------------------------------------------------------------- */

uint32_t sim_ms;
static uint32_t us_acc;
uint32_t hal_millis(void) { return sim_ms; }
void hal_delay_us(uint32_t us)
{
    us_acc += us;
    sim_ms += us_acc / 1000u;
    us_acc %= 1000u;
}

/* ---- pins: buttons, key matrix, ADC ----------------------------------- */

#define NPIN 80
static uint8_t level[NPIN];      /* what the outputs were last driven to */
static uint32_t btn_down;        /* bit i = button i pressed */
static uint8_t key_contacts[GM_MATRIX_KEYS];  /* bit 0/1 = contact 1/2 closed */
static int32_t adc_value = 2048;

void hal_pin_mode(uint16_t pin, uint8_t mode) { (void)pin; (void)mode; }
void hal_pin_write(uint16_t pin, int l) { if (pin < NPIN) level[pin] = (uint8_t)(l != 0); }

int hal_pin_read(uint16_t pin)
{
    for (uint32_t i = 0; i < gm_device.n_buttons; i++) {
        gm_pin_t p = gm_device.buttons[i].pin;
        if (!p.hub && p.num == pin)
            return (int)(((btn_down >> i) & 1u) ^ ((p.flags & GM_PIN_INVERT) ? 1u : 0u));
    }
    const gm_matrix_t *m = gm_device.matrix;
    for (uint32_t c = 0; m && c < m->n_cols; c++) {
        if (m->cols[c].hub || m->cols[c].num != pin)
            continue;
        /* velocity matrix: key r + rows * p is on row r, columns 2p (contact 1)
         * and 2p + 1 (contact 2); rows drive high, columns pulled down */
        int v = 0;
        for (uint32_t r = 0; r < m->n_rows; r++) {
            int on = level[m->rows[r].num] ^ ((m->rows[r].flags & GM_PIN_INVERT) ? 1 : 0);
            uint32_t k = r + m->n_rows * (c / 2u);
            if (on && k < m->n_keys && ((key_contacts[k] >> (c & 1u)) & 1u))
                v = 1;
        }
        return v ^ ((m->cols[c].flags & GM_PIN_INVERT) ? 1 : 0);
    }
    return 1;
}
int32_t hal_adc_read(uint16_t pin) { (void)pin; return adc_value; }
int hal_i2c_write(uint8_t addr, const uint8_t *p, uint32_t n) { (void)addr; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t addr, uint8_t *p, uint32_t n) { (void)addr; (void)p; (void)n; return -1; }

/* ---- MIDI out: [len lo, len hi, cable, bytes...] per message ---------- */

static uint8_t out[1 << 20];
static uint32_t out_n;
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    if (!n || out_n + n + 3 > sizeof out)
        return;
    out[out_n++] = (uint8_t)n;
    out[out_n++] = (uint8_t)(n >> 8);
    out[out_n++] = cable;
    memcpy(out + out_n, m, n);
    out_n += n;
}
void hal_midi_send(const uint8_t *m, uint32_t n) { hal_midi_send_cable(0, m, n); }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }

/* ---- identity, reboot, upload ---------------------------------------- */

void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0xA0 + i); }
static jmp_buf reboot_jb;
static int in_call;
int sim_reboot_mode;             /* 1 app, 2 bootloader (hal_reboot was called) */
void hal_reboot(int to_bootloader)
{
    sim_reboot_mode = to_bootloader ? 2 : 1;
    if (in_call)
        longjmp(reboot_jb, 1);
    abort();
}
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

/* ---- API for run_presets.py ------------------------------------------- */

int sim_booted;                  /* 0 in a freshly loaded library */

/* What main() does: the boot combo check, gm_init. */
void sim_boot(int force_safe)
{
    uint32_t combo = gm_device.boot_combo;
    int safe = force_safe || (combo && (gm_buttons_now() & combo) == combo);
    sim_booted++;
    gm_init(GM_FW_VERSION, safe);
}

uint32_t sim_take(uint8_t *buf, uint32_t cap)
{
    uint32_t n = out_n < cap ? out_n : cap;
    memcpy(buf, out, n);
    out_n = 0;
    return n;
}

/* Received MIDI bytes (a stream: serial-style, cable 0), as the platform
 * feeds them. Returns 1 if the device rebooted. */
int sim_feed(const uint8_t *p, uint32_t n)
{
    in_call = 1;
    if (setjmp(reboot_jb)) { in_call = 0; return 1; }
    for (uint32_t i = 0; i < n; i++)
        gm_midi_rx_byte(p[i]);
    in_call = 0;
    return 0;
}

/* ms of main loop: per ms, the scan ticks of one ms (scan_us) then gm_poll. */
static void run_ms(uint32_t ms)
{
    uint32_t ticks = gm_device.scan_us ? 1000u / gm_device.scan_us : 1u;
    for (uint32_t i = 0; i < ms; i++) {
        uint32_t t0 = sim_ms;
        for (uint32_t k = 0; k < ticks; k++)
            gm_scan_tick();
        sim_ms = t0 + 1u;       /* the scan's settle delays don't count */
        gm_poll();
    }
}
int sim_advance(uint32_t ms)
{
    in_call = 1;
    if (setjmp(reboot_jb)) { in_call = 0; return 1; }
    run_ms(ms);
    in_call = 0;
    return 0;
}

/* One scan tick (for key timing) without a main-loop pass. */
void sim_scan(uint32_t n) { for (uint32_t i = 0; i < n; i++) gm_scan_tick(); }

void sim_button(uint32_t i, int down)
{
    if (down)
        btn_down |= 1u << i;
    else
        btn_down &= ~(1u << i);
}
void sim_key(uint32_t k, uint32_t contacts) { if (k < GM_MATRIX_KEYS) key_contacts[k] = (uint8_t)contacts; }
void sim_adc(int32_t v) { adc_value = v; }
