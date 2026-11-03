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
void hal_midi_flush(uint32_t t) { (void)t; }
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

static void knob(int pin, int raw)
{
    adc[pin] = raw;
    run_ms(100);                        /* the input filter settles */
}

/* ---- tests ------------------------------------------------------------ */

int main(void)
{
    for (int i = 16; i < 20; i++)
        level[i] = 1;                   /* buttons released */
    adc[0] = 2048;                      /* knob at 64 */
    adc[1] = 0;                         /* fader at the bottom */
    gm_init(0x01020000u, 0);           /* fw 1.2.0 */
    run_ms(2000);
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

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
