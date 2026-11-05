/*
 * test_feedback.c: host test of the incoming-MIDI dispatcher, F1 host
 * feedback and Mackie Control, with the device core/test/feedback-test.yaml
 * (USB DAW port, MCU, LED sources and owners, pick-up, encoders, a V-Pot
 * ring). Simulated HAL: pins, ADC, MIDI out with the cable of each message.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_mcu.h"
#include "gm_settings.h"
#include "gm_feedback.h"
#include "app_proto.h"
#include "params.h"
#include "gsx.h"

#if !(GM_FEEDBACK && GM_MCU && GM_USB_DAW_PORT && GM_LED_FB && GM_ANALOG_PICKUP)
#error "test_feedback.c needs the feedback-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

static uint32_t now_ms;
static uint8_t level[64];
static int32_t adc[8];

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l) { level[pin] = (uint8_t)l; }
int hal_pin_read(uint16_t pin) { return level[pin]; }
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask) { (void)pin; (void)mask; return 0; }
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
int32_t hal_adc_read(uint16_t pin) { return pin < 8 ? adc[pin] : -1; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }

/* MIDI out: messages with their cable */
static uint8_t out[8192], out_cable[8192];
static int out_n;
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    if (out_n + (int)n < (int)sizeof out) {
        memcpy(out + out_n, m, n);
        memset(out_cable + out_n, cable, n);
        out_n += (int)n;
    }
}
void hal_midi_send(const uint8_t *m, uint32_t n) { hal_midi_send_cable(0, m, n); }
void hal_midi_poll(void) {}
static int flush_calls;
void hal_midi_flush(uint32_t t) { (void)t; flush_calls++; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

/* MCU display hooks (the weak ones in mcu.c do nothing) */
static char lcd_text[112 + 1];
static int lcd_calls;
static uint8_t char_seen[12];

void gm_mcu_on_lcd(uint32_t pos, const uint8_t *text, uint32_t n)
{
    for (uint32_t i = 0; i < n && pos + i < 112; i++)
        lcd_text[pos + i] = (char)text[i];
    lcd_calls++;
}

void gm_mcu_on_char(uint32_t pos, uint8_t v)
{
    if (pos < 12)
        char_seen[pos] = v;
}

/* ---- helpers ---------------------------------------------------------- */

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        gm_poll();
    }
}

/* One USB-MIDI event from the host. */
static void usb(uint8_t cable, uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint8_t cin = b0 >= 0xF8 ? 0x0F : (uint8_t)(b0 >> 4);
    uint8_t ev[4] = { (uint8_t)(cable << 4 | cin), b0, b1, b2 };
    gm_midi_rx_usb(ev);
}

/* A SysEx message from the host as USB-MIDI events (CIN 4..7). */
static void usb_sysex(uint8_t cable, const uint8_t *p, int n)
{
    while (n > 0) {
        int k = n > 3 ? 3 : n;
        uint8_t cin = (uint8_t)(n > 3 ? 4 : 4 + k);
        uint8_t ev[4] = { (uint8_t)(cable << 4 | cin), p[0], k > 1 ? p[1] : 0, k > 2 ? p[2] : 0 };
        gm_midi_rx_usb(ev);
        p += k;
        n -= k;
    }
}

/* Find a message with this status (and data 1, if d1 >= 0) on a cable;
 * returns its offset or -1. */
static int find(int cable, uint8_t status, int d1)
{
    for (int i = 0; i < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == status && out_cable[i] == cable && (d1 < 0 || (i + 1 < out_n && out[i + 1] == d1)))
            return i;
    }
    return -1;
}

/* The last such message; -1 if none. */
static int find_last(int cable, uint8_t status, int d1)
{
    int last = -1;
    for (int i = 0; i < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == status && out_cable[i] == cable && (d1 < 0 || (i + 1 < out_n && out[i + 1] == d1)))
            last = i;
    }
    return last;
}

static int count_status(uint8_t status_hi)
{
    int n = 0;
    for (int i = 0; i < out_n; i++)
        if ((out[i] & 0xF0) == status_hi)
            n++;
    return n;
}

/* buttons: PB0..PB3 (pins 16..19), active-low */
static void button(int i, int down)
{
    level[16 + i] = down ? 0 : 1;
    run_ms(15);
}

/* LEDs: PC0..PC8 = pins 32..40, ring PD0..PD4 = pins 48..52 (active-high) */
enum { PLAY, TOGGLE, MOM, PAD, ARM, BEAT, RUN, REC, CCL };
static int led(int i) { return level[32 + i]; }
static int ring_led(int k) { return level[48 + k]; }

/* encoder e (pins a = PB4 + 2e, b = PB5 + 2e): n detents, + = clockwise */
static void turn(int e, int n)
{
    static const uint8_t seq[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    int pa = 20 + 2 * e, pb = 21 + 2 * e;
    for (int d = 0; d < (n < 0 ? -n : n); d++) {
        for (int s = 1; s <= 4; s++) {
            int k = n > 0 ? s & 3 : (4 - s) & 3;
            level[pa] = seq[k][0];
            level[pb] = seq[k][1];
            run_ms(1);
        }
    }
    run_ms(2);
}

/* A gemifi request on the main port; returns the reply's status (or -1) and
 * points *pl at the reply payload after the status, *pn at its length. */
static int gsx_request(uint8_t cmd, const uint8_t *p, int n, const uint8_t **pl, int *pn)
{
    static uint8_t buf[256];
    static uint8_t seq = 20;
    gsx_tx_t t;
    seq = (uint8_t)((seq + 1) & 0x7F);
    gsx_tx_begin(&t, buf, cmd, seq);    /* a new seq each time: a repeat would be a resend */
    for (int i = 0; i < n; i++)
        gsx_put(&t, p[i]);
    uint16_t len = gsx_tx_end(&t);
    out_n = 0;
    usb_sysex(0, buf, len);
    for (int i = 0; i + 1 + GSX_OFF_PAYLOAD < out_n; i++)
        if (out[i] == 0xF0 && out[i + 1] == 0x7D && out[i + 1 + GSX_OFF_CMD] == (cmd | GSX_REPLY)) {
            const uint8_t *b = out + i + 1;
            int plen = b[GSX_OFF_LEN] | b[GSX_OFF_LEN + 1] << 7;
            if (pl)
                *pl = b + GSX_OFF_PAYLOAD + 1;
            if (pn)
                *pn = plen - 1;
            return b[GSX_OFF_PAYLOAD];
        }
    return -1;
}

/* The first gemifi message with this cmd in the output: offset of its F0, or -1. */
static int find_sysex(uint8_t cmd)
{
    for (int i = 0; i + 1 + GSX_OFF_PAYLOAD < out_n; i++)
        if (out[i] == 0xF0 && out[i + 1] == 0x7D && out[i + 1 + GSX_OFF_CMD] == cmd)
            return i;
    return -1;
}

/* An lstring at *p: compare with want, move past it. */
static int lstr_is(const uint8_t **p, const char *want)
{
    int n = (*p)[0], ok = n == (int)strlen(want) && memcmp(*p + 1, want, (size_t)n) == 0;
    *p += 1 + n;
    return ok;
}

static void knob(int pin, int raw)
{
    adc[pin] = raw;
    run_ms(150);                        /* the input filter settles (3 inputs, one per 2 ms) */
}

/* ---- tests ------------------------------------------------------------ */

int main(void)
{
    for (int i = 16; i < 20; i++)
        level[i] = 1;                   /* buttons released */
    adc[0] = 2048;                      /* knob at 64 */
    adc[1] = 0;                         /* fader at the bottom */
    gm_init(0x01020000u, 0);           /* fw 1.2.0 */
    run_ms(1400);
    CHECK(find_sysex(GSX_MSG_BOOTED) < 0, "no BOOTED before 1.5 s");
    run_ms(600);
    {
        /* BOOTED (sysex.md 4.6.3): HELLO's fields without the status */
        int b = find_sysex(GSX_MSG_BOOTED);
        uint8_t p[64] = { 0 };
        int plen = b < 0 ? 0 : out[b + 1 + GSX_OFF_LEN] | out[b + 1 + GSX_OFF_LEN + 1] << 7;
        if (b >= 0 && plen <= (int)sizeof p)
            memcpy(p, out + b + 1 + GSX_OFF_PAYLOAD, (size_t)plen);   /* gsx_request reuses out */
        CHECK(b >= 0 && plen == 27 + 1 + 13, "BOOTED once, 27 bytes + node feedback-test");
        CHECK(b >= 0 && p[0] == 0 && gsx_get_u(p + 1, 2) == 0x3F10 && gsx_get_u(p + 3, 5) == 0x01020000u &&
              p[8] == GSX_PROTO_VERSION && (gsx_get_u(p + 11, 2) & GSX_CAP_FEEDBACK) && p[27] == 13 &&
              memcmp(p + 28, "feedback-test", 13) == 0, "BOOTED: role, board, fw, caps bit 6, node");
        const uint8_t *h;
        int hn;
        CHECK(gsx_request(GSX_CMD_HELLO, 0, 0, &h, &hn) == GSX_OK && hn == plen && memcmp(h, p, (size_t)hn) == 0,
              "BOOTED = HELLO reply without the status");
        out_n = 0;
        run_ms(3000);
        CHECK(find_sysex(GSX_MSG_BOOTED) < 0, "BOOTED only once");
    }
    out_n = 0;

    /* button.action: the preset actions, then Mackie Control at 16 */
    {
        const param_def_t *act = params_find(P_BTN_ACTION);
        CHECK(act && act->option_count == 17 && act->max == ACT_MCU && ACT_MCU == 16 &&
              gm_cfg.btn_action[0] == ACT_MCU, "mcu action is 16, options 0..16");
        CHECK(GM_N_PRESETS == 1 && params_find(P_PRESET_ACTIVE) && !params_find(P_PRESET_PROGRAM),
              "one preset: preset.active only");
    }

    /* -- dispatcher: SysEx still reaches the gemifi protocol on cable 0 -- */
    {
        uint8_t buf[64];
        gsx_tx_t t;
        gsx_tx_begin(&t, buf, GSX_CMD_HELLO, 1);
        uint16_t len = gsx_tx_end(&t);
        usb_sysex(0, buf, len);
        int hello = -1;
        for (int i = 0; i + 5 < out_n; i++)
            if (out[i] == 0xF0 && out[i + 1] == 0x7D && out[i + 1 + GSX_OFF_CMD] == (GSX_CMD_HELLO | GSX_REPLY))
                hello = i;
        CHECK(hello >= 0 && out_cable[hello] == 0, "HELLO on cable 0 answered on cable 0");
        out_n = 0;
        gsx_tx_begin(&t, buf, GSX_CMD_HELLO, 2);
        len = gsx_tx_end(&t);
        usb_sysex(1, buf, len);
        CHECK(out_n == 0, "gemifi SysEx on the DAW port is not the gemifi protocol (%d bytes out)", out_n);
    }

    /* -- F1: cc_toggle follows the host's echo ------------------------- */
    out_n = 0;
    usb(0, 0xB0, 80, 127);
    run_ms(20);
    CHECK(led(TOGGLE) == 1, "toggle LED on from host CC 80 = 127");
    button(1, 1); button(1, 0);
    CHECK(find(0, 0xB0, 80) >= 0 && out[find(0, 0xB0, 80) + 2] == 0, "next press toggles off (CC 80 = 0)");
    run_ms(20);
    CHECK(led(TOGGLE) == 0, "toggle LED off after the press");
    usb(0, 0xB1, 80, 127);              /* other channel: not ours */
    run_ms(20);
    CHECK(led(TOGGLE) == 0, "CC on channel 2 ignored");

    /* -- F1: running status over a byte stream ------------------------- */
    {
        const uint8_t s[] = { 0xB0, 80, 127, 0xF8, 80, 0, 80, 127 };   /* clock in the middle */
        for (unsigned i = 0; i < sizeof s; i++)
            gm_midi_rx_byte(s[i]);
        run_ms(20);
        CHECK(led(TOGGLE) == 1, "byte stream: running status, realtime in between (last = 127)");
    }

    /* -- LED ownership: host_then_local with a 1 s timeout --------------- */
    usb(0, 0xB0, 64, 127);
    run_ms(20);
    CHECK(led(MOM) == 1, "momentary LED lit by the host (CC 64)");
    run_ms(500);
    CHECK(led(MOM) == 1, "still the host's after 0.5 s");
    run_ms(600);
    CHECK(led(MOM) == 0, "back to local (not held) after 1 s");
    button(2, 1);
    run_ms(20);
    CHECK(led(MOM) == 1, "local: lit while held");
    usb(0, 0xB0, 64, 0);
    run_ms(20);
    CHECK(led(MOM) == 0, "host says off: host wins while held");
    button(2, 0);
    usb(0, 0xB0, 64, 1);                /* 1 = blink */
    {
        int on = 0, off = 0;
        for (int i = 0; i < 100; i++) {
            run_ms(10);
            if (led(MOM)) on++; else off++;
        }
        CHECK(on > 20 && off > 20, "value 1 = blinking (%d on, %d off)", on, off);
    }
    /* owner local: the host's note doesn't touch it */
    usb(0, 0x90, 36, 127);
    run_ms(20);
    CHECK(led(PAD) == 0, "owner local: host note 36 ignored");

    /* -- explicit LED sources ------------------------------------------ */
    usb(0, 0x9F, 40, 127);
    run_ms(20);
    CHECK(led(ARM) == 1, "arm LED: note 40 on channel 16");
    usb(0, 0x90, 40, 0);
    run_ms(20);
    CHECK(led(ARM) == 1, "note off on channel 1 doesn't touch it");
    usb(0, 0x8F, 40, 0);
    run_ms(20);
    CHECK(led(ARM) == 0, "note off on channel 16: off");
    usb(0, 0xB0, 90, 100);
    run_ms(1500);
    CHECK(led(CCL) == 1, "no local state: the host keeps it past the timeout");

    /* clock and transport */
    usb(0, 0xFA, 0, 0);
    run_ms(20);
    CHECK(led(RUN) == 1, "Start: play LED on");
    usb(0, 0xF8, 0, 0);
    run_ms(20);
    CHECK(led(BEAT) == 1, "first clock of the beat: beat LED on");
    for (int i = 0; i < 6; i++)
        usb(0, 0xF8, 0, 0);
    run_ms(20);
    CHECK(led(BEAT) == 0, "after a 16th: off");
    for (int i = 0; i < 18; i++)
        usb(0, 0xF8, 0, 0);
    run_ms(20);
    CHECK(led(BEAT) == 1, "next beat (clock 25 = 24 later): on");
    usb(0, 0xFC, 0, 0);
    run_ms(20);
    CHECK(led(RUN) == 0 && led(BEAT) == 0, "Stop: play and beat LEDs off");

    /* -- pick-up ------------------------------------------------------- */
    out_n = 0;
    usb(0, 0xB0, 7, 100);               /* host's volume is 100; knob at 64 */
    knob(0, 80 * 32 + 16);
    CHECK(find(0, 0xB0, 7) < 0, "pick-up: knob at 80 < 100 stays silent");
    knob(0, 110 * 32 + 16);
    CHECK(find(0, 0xB0, 7) >= 0 && out[find(0, 0xB0, 7) + 2] >= 100 && out[find_last(0, 0xB0, 7) + 2] == 110,
          "reached 100: sends, up to 110 (first %d, last %d)", find(0, 0xB0, 7) >= 0 ? out[find(0, 0xB0, 7) + 2] : -1,
          find_last(0, 0xB0, 7) >= 0 ? out[find_last(0, 0xB0, 7) + 2] : -1);
    out_n = 0;
    knob(0, 90 * 32 + 16);
    CHECK(find(0, 0xB0, 7) >= 0, "picked up: follows again");

    /* -- absolute encoder takes the host's value ----------------------- */
    out_n = 0;
    usb(0, 0xB0, 30, 100);
    turn(0, 1);
    CHECK(find(0, 0xB0, 30) >= 0 && out[find(0, 0xB0, 30) + 2] == 101, "encoder: host 100, +1 -> 101 (got %d)",
          find(0, 0xB0, 30) >= 0 ? out[find(0, 0xB0, 30) + 2] : -1);

    /* -- feedback follows a CC that changed (the used-number map) ------- */
    {
        /* SET_PARAM encoder.cc[0] = 45: the host's CC 45 is the encoder's now */
        uint8_t sp[9] = { P_ENC_CC, 0, 0, 0, 45, 0, 0, 0, 0 };
        CHECK(gsx_request(GSX_CMD_SET_PARAM, sp, 9, 0, 0) == GSX_OK, "encoder.cc[0] = 45");
        usb(0, 0xB0, 99, 5);            /* nobody's CC: nothing changes */
        usb(0, 0xB0, 45, 20);
        out_n = 0;
        turn(0, 1);
        CHECK(find(0, 0xB0, 45) >= 0 && out[find(0, 0xB0, 45) + 2] == 21,
              "encoder on CC 45: host 20, +1 -> 21 (got %d)", find(0, 0xB0, 45) >= 0 ? out[find(0, 0xB0, 45) + 2] : -1);
        /* SET_PARAMS (writes the values itself) encoder.cc[0] = 46 */
        uint8_t sps[11] = { P_ENC_CC, 0, 0, 0, 1, 0, 46, 0, 0, 0, 0 };
        CHECK(gsx_request(GSX_CMD_SET_PARAMS, sps, 11, 0, 0) == GSX_OK, "SET_PARAMS encoder.cc[0] = 46");
        usb(0, 0xB0, 46, 70);
        out_n = 0;
        turn(0, -1);
        CHECK(find(0, 0xB0, 46) >= 0 && out[find(0, 0xB0, 46) + 2] == 69,
              "encoder on CC 46: host 70, -1 -> 69 (got %d)", find(0, 0xB0, 46) >= 0 ? out[find(0, 0xB0, 46) + 2] : -1);
        /* the toggle button on CC 81: its LED follows CC 81 */
        uint8_t sb[9] = { P_BTN_NUMBER, 0, TOGGLE, 0, 81, 0, 0, 0, 0 };
        CHECK(gsx_request(GSX_CMD_SET_PARAM, sb, 9, 0, 0) == GSX_OK, "button.number[toggle] = 81");
        usb(0, 0xB0, 81, 127);
        run_ms(20);
        CHECK(led(TOGGLE) == 1, "toggle LED on from host CC 81 = 127");
        usb(0, 0xB0, 81, 0);
        run_ms(20);
        CHECK(led(TOGGLE) == 0, "and off from CC 81 = 0");
        sb[4] = 80;
        gsx_request(GSX_CMD_SET_PARAM, sb, 9, 0, 0);
        sp[4] = 30;
        gsx_request(GSX_CMD_SET_PARAM, sp, 9, 0, 0);
    }

    /* -- MCU ----------------------------------------------------------- */
    out_n = 0;
    button(0, 1);
    CHECK(find(1, 0x90, 0x5E) >= 0 && out[find(1, 0x90, 0x5E) + 2] == 0x7F, "MCU play: Note On 5E 7F on cable 1");
    button(0, 0);
    CHECK(count_status(0x90) == 2 && find(0, 0x90, -1) < 0, "release: 5E 00, nothing on cable 0");
    usb(1, 0x90, 0x5E, 0x7F);
    run_ms(3000);
    CHECK(led(PLAY) == 1, "MCU play LED from the host, kept (owner host)");
    usb(1, 0x90, 0x5F, 0x01);
    {
        int on = 0, off = 0;
        for (int i = 0; i < 100; i++) {
            run_ms(10);
            if (led(REC)) on++; else off++;
        }
        CHECK(on > 20 && off > 20, "MCU record LED blinking (%d/%d)", on, off);
    }
    usb(0, 0x90, 0x5E, 0);              /* same note on the main port: not MCU */
    run_ms(20);
    CHECK(led(PLAY) == 1, "note 5E on cable 0 doesn't touch the MCU LED");

    /* version request */
    out_n = 0;
    {
        const uint8_t q[] = { 0xF0, 0x00, 0x00, 0x66, 0x14, 0x13, 0x00, 0xF7 };
        usb_sysex(1, q, sizeof q);
        const uint8_t want[] = { 0xF0, 0x00, 0x00, 0x66, 0x14, 0x14, 'V', '1', '.', '0', '2', 0xF7 };
        CHECK(out_n == (int)sizeof want && memcmp(out, want, sizeof want) == 0 && out_cable[0] == 1,
              "version reply V1.02 on cable 1 (%d bytes)", out_n);
        out_n = 0;
        usb_sysex(0, q, sizeof q);
        CHECK(out_n == 0, "no MCU reply to a request on the main port");
    }

    /* fader: pitch bend ch 1 on cable 1, pick-up from the host's position */
    out_n = 0;
    knob(1, 2048);
    {
        int i = find_last(1, 0xE0, -1);
        CHECK(i >= 0 && (out[i + 1] | out[i + 2] << 7) >= 8000 && (out[i + 1] | out[i + 2] << 7) <= 8400,
              "fader to the middle: pitch bend ~8192 (%d)", i >= 0 ? out[i + 1] | out[i + 2] << 7 : -1);
    }
    usb(1, 0xE0, 0, 120);               /* host: fader near the top */
    out_n = 0;
    knob(1, 3000);
    CHECK(find(1, 0xE0, -1) < 0, "fader below the host's position: silent");
    knob(1, 4095);
    CHECK(find(1, 0xE0, -1) >= 0, "crossed: sends again");

    /* V-Pot: CC 0x10 relative, ring CC 0x30 */
    out_n = 0;
    turn(1, 1);
    CHECK(find(1, 0xB0, 0x10) >= 0 && out[find(1, 0xB0, 0x10) + 2] == 0x01, "V-Pot +1: CC 10 01");
    out_n = 0;
    turn(1, -1);
    CHECK(find(1, 0xB0, 0x10) >= 0 && out[find(1, 0xB0, 0x10) + 2] == 0x41, "V-Pot -1: CC 10 41");
    usb(1, 0xB0, 0x30, 0x20 | 6);       /* wrap mode, position 6 of 11 */
    run_ms(20);
    CHECK(ring_led(0) && ring_led(1) && ring_led(2) && !ring_led(3) && !ring_led(4),
          "ring wrap 6/11: 3 of 5 lit (%d%d%d%d%d)", ring_led(0), ring_led(1), ring_led(2), ring_led(3), ring_led(4));
    usb(1, 0xB0, 0x30, 0x00 | 11);      /* dot at the right end */
    run_ms(20);
    CHECK(!ring_led(0) && !ring_led(3) && ring_led(4), "ring dot 11/11: last LED only");
    usb(1, 0xB0, 0x30, 0x10 | 1);       /* boost/cut at the far left */
    run_ms(20);
    CHECK(ring_led(0) && ring_led(1) && ring_led(2) && !ring_led(3), "ring boost/cut 1: centre to the left");
    usb(1, 0xB0, 0x30, 0x30 | 6);       /* spread, full */
    run_ms(20);
    CHECK(ring_led(0) && ring_led(4), "ring spread 6: all");
    CHECK(!ring_led(5), "centre LED off without bit 6");
    usb(1, 0xB0, 0x30, 0x40 | 0x00 | 6);
    run_ms(20);
    CHECK(ring_led(5) && ring_led(2), "bit 6: centre LED on, with the dot");

    /* timecode / assignment characters: CC 0x40 + n */
    usb(1, 0xB0, 0x4B, 0x13);           /* 'S' */
    usb(1, 0xB0, 0x40, 0x31 | 0x40);    /* '1' with its decimal point */
    CHECK(gm_mcu_char(11) == 0x13 && gm_mcu_char_ascii(gm_mcu_char(11)) == 'S' && char_seen[11] == 0x13,
          "character 11 = 'S'");
    CHECK(gm_mcu_char_ascii(gm_mcu_char(0)) == '1' && (gm_mcu_char(0) & 0x40), "character 0 = '1' + point");
    usb(0, 0xB0, 0x41, 0x05);
    CHECK(gm_mcu_char(1) == 0, "characters only on the DAW port");

    /* LCD text: SysEx 12, streamed to the hook */
    {
        uint8_t m[6 + 20 + 1] = { 0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 56 };
        const char *t = "Track 1 Name Here!!";
        for (int i = 0; i < 19; i++)
            m[7 + i] = (uint8_t)t[i];
        m[26] = 0xF7;
        lcd_calls = 0;
        usb_sysex(1, m, 27);
        CHECK(memcmp(lcd_text + 56, t, 19) == 0, "LCD text at row 2 ('%.19s')", lcd_text + 56);
        CHECK(lcd_calls >= 2, "LCD text came in pieces (%d calls)", lcd_calls);
    }

    /* meter mode: SysEx 20 */
    {
        const uint8_t m[] = { 0xF0, 0x00, 0x00, 0x66, 0x14, 0x20, 3, 0x05, 0xF7 };
        usb_sysex(1, m, sizeof m);
        CHECK(gm_mcu_meter_mode(3) == 5, "meter mode of strip 4 = 5");
    }

    /* handshake: 00 -> 01 serial challenge; 02 serial response -> 03 serial */
    {
        const uint8_t q[] = { 0xF0, 0x00, 0x00, 0x66, 0x14, 0x00, 0xF7 };
        out_n = 0;
        usb_sysex(1, q, sizeof q);
        CHECK(out_n == 18 && out[5] == 0x01 && out[17] == 0xF7 && out_cable[0] == 1,
              "device query: 01 + 7 serial + 4 challenge (%d bytes)", out_n);
        uint8_t serial[7];
        memcpy(serial, out + 6, 7);
        uint8_t a[6 + 7 + 4 + 1] = { 0xF0, 0x00, 0x00, 0x66, 0x14, 0x02 };
        memcpy(a + 6, serial, 7);
        a[17] = 0xF7;
        out_n = 0;
        usb_sysex(1, a, sizeof a);
        CHECK(out_n == 14 && out[5] == 0x03 && memcmp(out + 6, serial, 7) == 0, "answer accepted: 03 + serial");
    }

    /* 62: all LEDs off */
    {
        const uint8_t m[] = { 0xF0, 0x00, 0x00, 0x66, 0x14, 0x62, 0xF7 };
        usb(1, 0x90, 0x5E, 0x7F);       /* play LED on */
        run_ms(20);
        usb_sysex(1, m, sizeof m);
        run_ms(20);
        CHECK(!ring_led(5) && !ring_led(2), "62: ring and centre off");
    }

    /* -- DAW-script messages (sysex.md 4.6) -------------------------------- */
    {
        const uint8_t *r;
        int rn;
        uint8_t q[3] = { GSX_KIND_BUTTON, 1, 0 };
        CHECK(gsx_request(GSX_CMD_GET_COMPONENT, q, 3, &r, &rn) == GSX_OK, "GET_COMPONENT button 1");
        CHECK(r[0] == 1 && gsx_get_u(r + 1, 2) == 1 && gsx_get_u(r + 3, 2) == 4, "kind 1, index 1, count 4");
        r += 5;
        CHECK(lstr_is(&r, "toggle"), "id toggle");
        r += 1 + r[0];                          /* name */
        CHECK(lstr_is(&r, "track_mute"), "role from daw:");
        CHECK(gsx_get_u(r, 2) == 1 && r[2] == 0, "its LED is LED 1; layer 0");

        q[1] = 0;
        gsx_request(GSX_CMD_GET_COMPONENT, q, 3, &r, &rn);
        r += 5;
        r += 1 + r[0];
        r += 1 + r[0];
        CHECK(lstr_is(&r, "play"), "role falls back to the mcu: name");

        uint8_t qe[3] = { GSX_KIND_ENCODER, 2, 0 };
        CHECK(gsx_request(GSX_CMD_GET_COMPONENT, qe, 3, &r, &rn) == GSX_OK, "GET_COMPONENT encoder 2");
        r += 5;
        CHECK(lstr_is(&r, "dial"), "id dial");
        r += 1 + r[0];
        CHECK(lstr_is(&r, "device_param_1"), "role device_param_1");
        CHECK(r[0] == 3 && gsx_get_u(r + 1, 2) == GSX_NONE_INDEX, "ring of 3, no centre");
        CHECK(r[3] == ENC_ABSOLUTE && r[4] == 0 && r + 5 == out + find_sysex(GSX_CMD_GET_COMPONENT | GSX_REPLY) + 1 +
              GSX_OFF_PAYLOAD + 1 + rn, "mode 0 (absolute), then layer 0 (not a twin), the last field");
        /* `mode` follows encoder.mode */
        uint8_t sp[9] = { P_ENC_MODE, 0, 2, 0, ENC_REL_OFFSET, 0, 0, 0, 0 };
        CHECK(gsx_request(GSX_CMD_SET_PARAM, sp, 9, 0, 0) == GSX_OK, "encoder.mode[2] = relative offset");
        gsx_request(GSX_CMD_GET_COMPONENT, qe, 3, &r, &rn);
        CHECK(rn >= 5 && r[rn - 2] == ENC_REL_OFFSET, "mode 2 after SET_PARAM");
        sp[4] = ENC_ABSOLUTE;
        gsx_request(GSX_CMD_SET_PARAM, sp, 9, 0, 0);
        qe[1] = 1;
        gsx_request(GSX_CMD_GET_COMPONENT, qe, 3, &r, &rn);
        r += 5;
        r += 1 + r[0];
        r += 1 + r[0];
        CHECK(lstr_is(&r, "vpot_1") && r[0] == 5 && gsx_get_u(r + 1, 2) == 14, "pan: vpot_1, ring 5, centre 14");
        CHECK(r[3] == GSX_ENC_MODE_MCU, "a V-Pot reports mode 3");
        qe[1] = 0;
        gsx_request(GSX_CMD_GET_COMPONENT, qe, 3, &r, &rn);
        CHECK(rn >= 5 && r[rn - 2] == ENC_ABSOLUTE, "enc: mode 0");

        uint8_t ql[3] = { GSX_KIND_LED, 15, 0 };
        CHECK(gsx_request(GSX_CMD_GET_COMPONENT, ql, 3, &r, &rn) == GSX_OK, "GET_COMPONENT LED 15");
        r += 5;
        CHECK(lstr_is(&r, "bi") && lstr_is(&r, "Bicolour"), "id bi, name Bicolour");
        r += 1 + r[0];
        CHECK(r[0] == 2 && r[1] == GM_OWNER_HOST_THEN_LOCAL && r[2] == 0, "two colours, owner host_then_local, layer 0");
        /* `owner`: the LED's own, MCU-driven ones host (sysex.md 4.6.1) */
        static const struct { uint8_t led, owner; } owners[] = {
            { PLAY, GM_OWNER_HOST },            /* the play button's mcu: LED */
            { PAD, GM_OWNER_LOCAL }, { MOM, GM_OWNER_HOST_THEN_LOCAL }, { REC, GM_OWNER_HOST },
            { 9, GM_OWNER_HOST }, { 14, GM_OWNER_HOST }, { 16, GM_OWNER_HOST },   /* rings, centre */
        };
        for (unsigned k = 0; k < sizeof owners / sizeof owners[0]; k++) {
            ql[0] = GSX_KIND_LED;
            ql[1] = owners[k].led;
            CHECK(gsx_request(GSX_CMD_GET_COMPONENT, ql, 3, &r, &rn) == GSX_OK && r[rn - 2] == owners[k].owner,
                  "LED %d owner %d (got %d)", owners[k].led, owners[k].owner, r[rn - 2]);
        }

        uint8_t qa[3] = { GSX_KIND_ANALOG, 1, 0 };
        gsx_request(GSX_CMD_GET_COMPONENT, qa, 3, &r, &rn);
        r += 5;
        r += 1 + r[0];
        r += 1 + r[0];
        CHECK(lstr_is(&r, "fader_1"), "fader role fader_1");
        CHECK(rn >= 1 && r[0] == 1, "fader: takeover pick-up (the MCU default)");
        qa[1] = 0;
        gsx_request(GSX_CMD_GET_COMPONENT, qa, 3, &r, &rn);
        CHECK(r[rn - 2] == 1 && r[rn - 1] == 0, "knob: takeover pick-up, then touch none");
        qa[1] = 2;
        gsx_request(GSX_CMD_GET_COMPONENT, qa, 3, &r, &rn);
        CHECK(gsx_get_u(r + 3, 2) == 3 && r[rn - 2] == 0 && r[rn - 1] == 0, "level: 3 analogs, takeover none");

        /* no pads, no meters (kind 8 is reserved): BAD_PARAM, as for any kind with none */
        uint8_t bad[][3] = { { GSX_KIND_BUTTON, 4, 0 }, { GSX_KIND_MATRIX, 0, 0 }, { GSX_KIND_DISPLAY, 0, 0 },
                             { GSX_KIND_PAD, 0, 0 }, { GSX_KIND_METER, 0, 0 }, { 9, 0, 0 } };
        for (int k = 0; k < 4; k++)
            CHECK(gsx_request(GSX_CMD_GET_COMPONENT, bad[k], 3, 0, 0) == GSX_BAD_PARAM, "BAD_PARAM %d", k);
        CHECK(gsx_request(GSX_CMD_GET_COMPONENT, q, 2, 0, 0) == GSX_BAD_LEN, "BAD_LEN");

        /* FB_LED: lit, no reply, and no timeout (led_timeout is 1 s here) */
        uint8_t m[8] = { MOM, 0, 127 };
        CHECK(gsx_request(GSX_FB_LED, m, 3, 0, 0) == -1 && out_n == 0, "FB_LED: no reply at all");
        run_ms(20);
        CHECK(led(MOM) == 1, "FB_LED lights the momentary LED");
        run_ms(1500);
        CHECK(led(MOM) == 1, "still lit after the 1 s timeout: script states don't time out");
        uint8_t rel[1] = { 1 };
        gsx_request(GSX_FB_RELEASE, rel, 1, 0, 0);
        run_ms(20);
        CHECK(led(MOM) == 0 && out_n == 0, "FB_RELEASE 1: back to local");

        /* bicolour: 2..63 colour 1 (PC9), 64..127 colour 2 (PC10) */
        uint8_t b1[3] = { 15, 0, 30 };
        gsx_request(GSX_FB_LED, b1, 3, 0, 0);
        run_ms(20);
        CHECK(led(9) == 1 && led(10) == 0, "level 30: colour 1");
        b1[2] = 100;
        gsx_request(GSX_FB_LED, b1, 3, 0, 0);
        run_ms(20);
        CHECK(led(9) == 0 && led(10) == 1, "level 100: colour 2");
        uint8_t rgb[6] = { 15, 0, 100, 0, 0, 0 };
        gsx_request(GSX_FB_LED_RGB, rgb, 6, 0, 0);
        run_ms(20);
        CHECK(led(9) == 1 && led(10) == 0, "RGB red: colour 1");
        uint8_t leds[6] = { 16, 0, 3, 127, 0, 127 };
        gsx_request(GSX_FB_LEDS, leds, 6, 0, 0);
        run_ms(20);
        CHECK(ring_led(6) && !ring_led(7) && ring_led(8), "FB_LEDS: 3 LEDs at once");

        /* FB_RING on an encoder without Mackie Control */
        uint8_t ring[6] = { 2, 0, 2, 0x7F, 0x7F, 0 };   /* wrap, full */
        gsx_request(GSX_FB_RING, ring, 6, 0, 0);
        run_ms(20);
        CHECK(ring_led(6) && ring_led(7) && ring_led(8), "FB_RING wrap full: all 3");
        uint8_t dot[6] = { 2, 0, 0, 0, 0, 0 };
        gsx_request(GSX_FB_RING, dot, 6, 0, 0);
        run_ms(20);
        CHECK(ring_led(6) && !ring_led(7) && !ring_led(8), "FB_RING dot at 0: first LED");
        uint8_t off[6] = { 2, 0, 0, 0, 0, 2 };
        gsx_request(GSX_FB_RING, off, 6, 0, 0);
        run_ms(20);
        CHECK(!ring_led(6) && !ring_led(7) && !ring_led(8), "FB_RING flags bit 1: ring off");

        /* FB_VALUE: an absolute encoder carries on from the host's value */
        uint8_t val[5] = { GSX_KIND_ENCODER, 2, 0, 0, 64 };   /* 64 << 7 */
        gsx_request(GSX_FB_VALUE, val, 5, 0, 0);
        out_n = 0;
        turn(2, 1);
        CHECK(find(0, 0xB0, 31) >= 0 && out[find(0, 0xB0, 31) + 2] == 65, "encoder continues at 65");

        /* a broken notification: still no reply */
        uint8_t buf[32];
        gsx_tx_t t;
        gsx_tx_begin(&t, buf, GSX_FB_LED, 1);
        gsx_put(&t, 0);
        uint16_t len = gsx_tx_end(&t);
        buf[len - 2] ^= 1;                      /* checksum */
        out_n = 0;
        usb_sysex(0, buf, len);
        CHECK(out_n == 0, "bad checksum on a notification: no reply");
    }

    /* -- wire cost, replies that don't wait, bursts (daw-scripts.md 5.12) -- */
    {
        /* frame sizes the docs quote: 11 bytes of framing + the payload */
        uint8_t buf[256];
        gsx_tx_t t;
        gsx_tx_begin(&t, buf, GSX_FB_LED, 0);
        CHECK(gsx_tx_end(&t) == 11, "framing is 11 bytes");
        out_n = 0;
        gm_cfg.events = 1;
        turn(2, 1);
        int ev = find_sysex(GSX_MSG_EVENT), ev_end = ev;
        while (ev_end >= 0 && ev_end < out_n && out[ev_end] != 0xF7)
            ev_end++;
        CHECK(ev >= 0 && ev_end - ev + 1 == 20, "EVENT is 20 bytes (%d)", ev_end - ev + 1);
        gm_cfg.events = 0;

        /* replies are queued, never flushed (the main loop sends them) */
        flush_calls = 0;
        CHECK(gsx_request(GSX_CMD_HELLO, 0, 0, 0, 0) == GSX_OK && flush_calls == 0, "HELLO reply without a flush");
        uint8_t qb[3] = { GSX_KIND_BUTTON, 9, 0 };
        CHECK(gsx_request(GSX_CMD_GET_COMPONENT, qb, 3, 0, 0) == GSX_BAD_PARAM && flush_calls == 0,
              "error reply without a flush");

        /* requests back to back, before any main-loop pass: each answered,
         * in order, with its own seq */
        static const uint8_t ask[4][3] = { { GSX_KIND_ENCODER, 0, 0 }, { GSX_KIND_ENCODER, 1, 0 },
                                           { GSX_KIND_ENCODER, 2, 0 }, { GSX_KIND_LED, 15, 0 } };
        int n = 0;
        for (int k = 0; k < 4; k++) {
            gsx_tx_begin(&t, buf + n, GSX_CMD_GET_COMPONENT, (uint8_t)(90 + k));
            for (int i = 0; i < 3; i++)
                gsx_put(&t, ask[k][i]);
            n += gsx_tx_end(&t);
        }
        out_n = 0;
        usb_sysex(0, buf, n);
        int got = 0, in_order = 1;
        for (int i = 0; i + 1 + GSX_OFF_PAYLOAD + 3 < out_n; i++)
            if (out[i] == 0xF0 && out[i + 1 + GSX_OFF_CMD] == (GSX_CMD_GET_COMPONENT | GSX_REPLY)) {
                const uint8_t *b = out + i + 1;
                if (b[GSX_OFF_SEQ] != 90 + got || b[GSX_OFF_PAYLOAD] != GSX_OK ||
                    b[GSX_OFF_PAYLOAD + 1] != ask[got][0] || b[GSX_OFF_PAYLOAD + 2] != ask[got][1])
                    in_order = 0;
                got++;
            }
        CHECK(got == 4 && in_order, "4 requests in one burst: 4 replies in order (%d)", got);

        /* a refresh burst of notifications, then one pass applies it */
        static const uint8_t burst[][8] = {
            { GSX_FB_RING, 6, 2, 0, 2, 0x7F, 0x7F, 0 },       /* encoder 2 wrap full: LEDs 6..8 */
            { GSX_FB_RING, 6, 2, 0, 0, 0x00, 0x40, 0 },       /* then dot at the middle: LED 7 */
            { GSX_FB_VALUE, 5, GSX_KIND_ENCODER, 2, 0, 0, 10 },   /* encoder 2 = 10 << 7 */
            { GSX_FB_LED, 3, MOM, 0, 127 },
        };
        n = 0;
        for (unsigned k = 0; k < sizeof burst / sizeof burst[0]; k++) {
            gsx_tx_begin(&t, buf + n, burst[k][0], 0);
            for (int i = 0; i < burst[k][1]; i++)
                gsx_put(&t, burst[k][2 + i]);
            n += gsx_tx_end(&t);
        }
        CHECK(n == 17 + 17 + 16 + 14, "FB_RING 17, FB_VALUE 16, FB_LED 14 bytes");
        out_n = 0;
        usb_sysex(0, buf, n);
        CHECK(out_n == 0, "notifications: nothing sent back");
        run_ms(20);
        CHECK(!ring_led(6) && ring_led(7) && !ring_led(8) && led(MOM) == 1, "the burst's last states shown");
        turn(2, 1);
        CHECK(find(0, 0xB0, 31) >= 0 && out[find(0, 0xB0, 31) + 2] == 11, "encoder continues at 11");
        uint8_t rel[1] = { 0 };
        gsx_request(GSX_FB_RELEASE, rel, 1, 0, 0);
        run_ms(20);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
