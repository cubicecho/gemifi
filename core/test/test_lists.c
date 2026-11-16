/*
 * test_lists.c: host test of the buttons' action lists (components/buttons/
 * README.md "Action lists") with core/test/lists-test.yaml, on fake time: a
 * list's order and channels, the releases, a CC with a fixed value, a
 * panic in a list, a list on a hold:, a combo and a layer twin, the
 * per-preset lists, and a preset switch while a list is held (nothing
 * hangs). A button without a list sends as before; EVENTs come at the
 * press and the release; host feedback leaves a list button alone.
 *
 * Buttons (PB0.., inverted): 0 multi [program 5 ch 2, CC 7 = 100, CC 64,
 * note 36 ch 10, MMC play], 1 plain CC 10, 2 chan (channel 3) [CC 30, note
 * 60 ch 4, SysEx vol, program 3 bank 130], 3 pan [note 40, panic, program
 * 9], 4 held CC 20 / held [CC 21, note 50], 5 fn: the modifier, plain's twin
 * [CC 12, CC 13 = 5], 6 c1 CC 41, 7 c2 CC 42; c1 + c2 [CC 43, note 70].
 * Preset 2: multi [CC 90, note 37 ch 5], plain [program 1, CC 11 = 64];
 * preset 3: multi note 61. hold_time 300 ms, combo_time 50 ms.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_controls.h"
#include "gm_device.h"
#include "gm_presets.h"
#include "gm_settings.h"

#if GM_N_BUTTONS != 9 || GM_BTN_LISTS != 8 || GM_N_BTN_ITEMS != 22 || GM_N_PRESETS != 3
#error "test_lists.c needs the lists-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

static uint32_t now_ms;
static uint8_t level[64];

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l) { level[pin] = (uint8_t)l; }
int hal_pin_read(uint16_t pin) { return level[pin]; }
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask) { (void)pin; (void)mask; return 0; }
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return 0; }
int hal_i2c_write(uint8_t bus, uint8_t a, const uint8_t *p, uint32_t n) { (void)bus; (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t bus, uint8_t a, uint8_t *p, uint32_t n) { (void)bus; (void)a; (void)p; (void)n; return -1; }

static uint8_t out[8192];
static int out_n;
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    if (cable == 0 && out_n + (int)n < (int)sizeof out) {
        memcpy(out + out_n, m, n);
        out_n += (int)n;
    }
}
void hal_midi_send(const uint8_t *m, uint32_t n) { hal_midi_send_cable(0, m, n); }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

/* ---- helpers ---------------------------------------------------------- */

enum { MULTI, PLAIN, CHAN, PAN, HELD, FN, C1, C2 };
#define PIN_BTN(b) (16 + (b))       /* PB0.. */
/* A release counts once the contact has read open for BTN_STABLE (16)
 * samples: let go, then this long until it is seen. */
#define RELEASE_MS 20u
#define HOLD_MS 300u                /* button_settings.hold_time */
#define COMBO_MS 50u                /* button_settings.combo_time */
#define GSX_SYSEX_ID 0x7D           /* gemifi's frames (EVENTs): F0 7D ... */

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        gm_poll();
    }
}

static void press(int b)
{
    level[PIN_BTN(b)] = 0;          /* inverted */
    run_ms(1);
}

static void release(int b)
{
    level[PIN_BTN(b)] = 1;
    run_ms(RELEASE_MS);
}

/* What went out since the last clear, in order, one word per message:
 * cc<ch>:<num>=<value>, on<ch>:<note>, off<ch>:<note>, pc<ch>:<program>,
 * mmc:<command>, sx (another SysEx string); channels 1..16. EVENTs
 * (gemifi's own frames) are left out; events() counts them. */
static const char *sent(void)
{
    static char s[2048];
    int j = 0;
    s[0] = 0;
    for (int i = 0; i < out_n && j < (int)sizeof s - 32;) {
        uint8_t st = out[i];
        if (st == 0xF0) {
            int start = i;
            while (i < out_n && out[i] != 0xF7)
                i++;
            i++;
            if (out[start + 1] == GSX_SYSEX_ID)
                continue;
            if (out[start + 1] == 0x7F && out[start + 3] == 0x06)
                j += snprintf(s + j, sizeof s - (size_t)j, "%smmc:%d", j ? " " : "", out[start + 4]);
            else
                j += snprintf(s + j, sizeof s - (size_t)j, "%ssx", j ? " " : "");
            continue;
        }
        int ch = (st & 0x0F) + 1;
        switch (st & 0xF0) {
        case 0xB0:
            j += snprintf(s + j, sizeof s - (size_t)j, "%scc%d:%d=%d", j ? " " : "", ch, out[i + 1], out[i + 2]);
            i += 3;
            break;
        case 0x90:
            j += snprintf(s + j, sizeof s - (size_t)j, "%s%s%d:%d", j ? " " : "", out[i + 2] ? "on" : "off", ch,
                          out[i + 1]);
            i += 3;
            break;
        case 0x80:
            j += snprintf(s + j, sizeof s - (size_t)j, "%soff%d:%d", j ? " " : "", ch, out[i + 1]);
            i += 3;
            break;
        case 0xC0:
            j += snprintf(s + j, sizeof s - (size_t)j, "%spc%d:%d", j ? " " : "", ch, out[i + 1]);
            i += 2;
            break;
        default:
            j += snprintf(s + j, sizeof s - (size_t)j, "%s?%02X", j ? " " : "", st);
            i++;
            break;
        }
    }
    return s;
}

/* gemifi's own SysEx frames (EVENTs) since the last clear. */
static int events(void)
{
    int n = 0;
    for (int i = 0; i + 1 < out_n; i++)
        if (out[i] == 0xF0 && out[i + 1] == GSX_SYSEX_ID)
            n++;
    return n;
}

#define SENT(want, what) CHECK(!strcmp(sent(), (want)), "%s: sent \"%s\", want \"%s\"", (what), sent(), (want))

/* ---- the tests -------------------------------------------------------- */

static void test_plain(void)
{
    out_n = 0;
    press(PLAIN);
    SENT("cc1:10=127", "plain (no list) pressed: CC 10 on, as before");
    out_n = 0;
    release(PLAIN);
    SENT("cc1:10=0", "plain released: CC 10 off");
}

static void test_order_and_channels(void)
{
    out_n = 0;
    press(MULTI);
    SENT("pc2:5 cc1:7=100 cc1:64=127 on10:36 mmc:2", "multi pressed: its list in order, each on its channel");
    out_n = 0;
    run_ms(500);
    SENT("", "held: nothing more");
    release(MULTI);
    SENT("cc1:64=0 off10:36", "multi released: CC 64 and note 36 let go; the fixed CC 7, program, MMC send nothing");
}

static void test_button_channel(void)
{
    out_n = 0;
    press(CHAN);
    SENT("cc3:30=127 on4:60 sx cc3:0=1 cc3:32=2 pc3:3",
         "chan pressed: the button's channel 3 where an item has none, its bank 130 = 1 / 2 before program 3");
    out_n = 0;
    release(CHAN);
    SENT("cc3:30=0 off4:60 sx", "chan released: CC 30 off, note 60 off, the SysEx string's value back to 0");
}

static void test_panic(void)
{
    out_n = 0;
    press(MULTI);
    press(PAN);
    const char *s = sent();
    const char *want_start = "pc2:5 cc1:7=100 cc1:64=127 on10:36 mmc:2 on1:40 cc1:64=0 off10:36 off1:40 cc1:120=0";
    CHECK(!strncmp(s, want_start, strlen(want_start)),
          "pan: note 40, then the panic lets go of multi's list and its own note 40 first (got \"%.120s\")", s);
    const char *tail = " cc16:121=0 pc1:9";
    CHECK(strlen(s) > strlen(tail) && !strcmp(s + strlen(s) - strlen(tail), tail),
          "pan: program 9 after the panic's All Notes Off on every channel");
    out_n = 0;
    release(PAN);
    release(MULTI);
    SENT("", "pan and multi released after the panic: silent");
}

static void test_hold_list(void)
{
    out_n = 0;
    press(HELD);
    release(HELD);
    SENT("cc1:20=127 cc1:20=0", "held tapped: its own CC 20, on and off");
    out_n = 0;
    press(HELD);
    run_ms(HOLD_MS + 10u);
    SENT("cc1:21=127 on1:50", "held held past hold_time: its hold: list");
    out_n = 0;
    release(HELD);
    SENT("cc1:21=0 off1:50", "released: the hold list let go");
}

static void test_twin_list(void)
{
    out_n = 0;
    press(FN);
    press(PLAIN);
    SENT("cc1:12=127 cc1:13=5", "fn + plain: the twin's list");
    out_n = 0;
    release(PLAIN);
    SENT("cc1:12=0", "plain released: the twin's CC 12 off (CC 13's fixed value sends nothing)");
    release(FN);
    out_n = 0;
    press(PLAIN);
    release(PLAIN);
    SENT("cc1:10=127 cc1:10=0", "plain without fn: its own CC 10 again");
}

static void test_combo_list(void)
{
    out_n = 0;
    press(C1);
    press(C2);
    SENT("cc1:43=127 on1:70", "c1 + c2: the combo's list");
    out_n = 0;
    release(C1);
    SENT("cc1:43=0 off1:70", "c1 released: the combo's list let go");
    out_n = 0;
    release(C2);
    run_ms(COMBO_MS);
    SENT("", "c2 released: silent");
}

static void test_presets(void)
{
    presets_select(2);
    out_n = 0;
    press(MULTI);
    SENT("cc1:90=127 on5:37", "preset 2: multi's own list there");
    out_n = 0;
    release(MULTI);
    SENT("cc1:90=0 off5:37", "preset 2: multi released");
    out_n = 0;
    press(PLAIN);
    SENT("pc1:1 cc1:11=64", "preset 2: plain sends a list");
    out_n = 0;
    release(PLAIN);
    SENT("", "preset 2: plain's list holds nothing");

    presets_select(3);
    out_n = 0;
    press(MULTI);
    release(MULTI);
    SENT("on1:61 off1:61", "preset 3: multi is note 61");
    presets_select(1);
    out_n = 0;
    press(MULTI);
    release(MULTI);
    SENT("pc2:5 cc1:7=100 cc1:64=127 on10:36 mmc:2 cc1:64=0 off10:36", "preset 1 again: multi's YAML list");
}

static void test_switch_while_held(void)
{
    out_n = 0;
    press(MULTI);
    press(HELD);
    run_ms(HOLD_MS + 10u);
    press(C1);
    press(C2);
    out_n = 0;
    presets_select(2);
    SENT("cc1:21=0 off1:50 cc1:43=0 off1:70 cc1:64=0 off10:36",
         "preset switch: the hold's, the combo's and multi's lists let go, each once");
    out_n = 0;
    release(C1);
    release(C2);
    release(HELD);
    release(MULTI);
    run_ms(COMBO_MS);
    SENT("", "released after the switch: silent");

    /* held through a switch to a preset where the button has another list,
     * then one where it has one action */
    out_n = 0;
    press(MULTI);
    SENT("cc1:90=127 on5:37", "preset 2: multi's list");
    out_n = 0;
    presets_select(3);
    SENT("cc1:90=0 off5:37", "switch to preset 3: preset 2's list let go");
    out_n = 0;
    release(MULTI);
    SENT("", "released: silent");
    out_n = 0;
    press(MULTI);
    release(MULTI);
    SENT("on1:61 off1:61", "preset 3: note 61");
    presets_select(1);
}

static void test_host_changes(void)
{
    /* the host gives multi one action while its list is held: the list is let go at the release anyway */
    out_n = 0;
    press(MULTI);
    gm_cfg.btn_action[MULTI] = ACT_CC_MOMENTARY;
    gm_cfg.btn_number[MULTI] = 99;
    out_n = 0;
    release(MULTI);
    SENT("cc1:64=0 off10:36 cc1:99=0", "host changed the action while held: the list still let go");

    /* a list number the device doesn't have sends nothing */
    gm_cfg.btn_action[MULTI] = ACT_LIST;
    gm_cfg.btn_number[MULTI] = GM_BTN_LISTS;
    out_n = 0;
    press(MULTI);
    release(MULTI);
    SENT("", "list number out of range: nothing");

    /* the host points multi at another of the lists: chan's */
    gm_cfg.btn_number[MULTI] = 1;
    out_n = 0;
    press(MULTI);
    release(MULTI);
    SENT("cc1:30=127 on4:60 sx cc1:0=1 cc1:32=2 pc1:3 cc1:30=0 off4:60 sx",
         "button.number 1: chan's list, on multi's channel");
    gm_cfg.btn_number[MULTI] = 0;
}

static void test_events_and_feedback(void)
{
    gm_cfg.events = 1;
    out_n = 0;
    press(MULTI);
    CHECK(events() == 1, "EVENT at the press of a list button (%d)", events());
    out_n = 0;
    release(MULTI);
    CHECK(events() == 1, "EVENT at the release (%d)", events());
    gm_cfg.events = 0;

    /* host CC 0 (multi's button.number, its list) and CC 64 (an item's): no toggle, nothing sent */
    const uint8_t cc0[4] = { 0x0B, 0xB0, 0, 127 }, cc64[4] = { 0x0B, 0xB0, 64, 127 };
    out_n = 0;
    gm_midi_rx_usb(cc0);
    gm_midi_rx_usb(cc64);
    run_ms(5);
    CHECK(btn_toggled == 0 && sent()[0] == 0, "host feedback leaves a list button alone");
    out_n = 0;
    press(MULTI);
    release(MULTI);
    SENT("pc2:5 cc1:7=100 cc1:64=127 on10:36 mmc:2 cc1:64=0 off10:36", "and it sends its list as before");
}

int main(void)
{
    for (int i = 0; i < 64; i++)
        level[i] = 1;               /* buttons up (inverted) */
    gm_init(0x00010000u, 0);
    run_ms(2000);

    test_plain();
    test_order_and_channels();
    test_button_channel();
    test_panic();
    test_hold_list();
    test_twin_list();
    test_combo_list();
    test_presets();
    test_switch_while_held();
    test_host_changes();
    test_events_and_feedback();

    printf("test_lists: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
