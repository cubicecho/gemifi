/*
 * hal_esp.cpp: time, pins, ADC, I2C, identity, reboot and the scan timer
 * (gm_hal.h) for ESP32 and ESP8266.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_esp.h"
#include <Wire.h>
#include "gm_hal.h"
#include "gm_safety.h"

#ifdef ESP32
#include <esp_mac.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <esp_system.h>
#else
extern "C" {
#include <user_interface.h>
}
#endif

/* ---- time ------------------------------------------------------------- */

uint32_t hal_millis(void)
{
    return millis();
}

void hal_delay_us(uint32_t us)
{
    if (us >= 10000) {            /* long waits yield (watchdog, USB, Wi-Fi) */
        delay(us / 1000);
        us %= 1000;
    }
    if (us)
        delayMicroseconds(us);
}

/* ---- pins ------------------------------------------------------------- */

void hal_pin_mode(uint16_t pin, uint8_t mode)
{
    switch (mode) {
    case HAL_IN_PULLUP:
        pinMode(pin, INPUT_PULLUP);
        break;
    case HAL_IN_PULLDOWN:
#ifdef ESP32
        pinMode(pin, INPUT_PULLDOWN);
#else
        pinMode(pin, pin == 16 ? INPUT_PULLDOWN_16 : INPUT);   /* only GPIO16 */
#endif
        break;
    case HAL_OUT:
        pinMode(pin, OUTPUT);
        break;
    case HAL_OUT_OD:
        pinMode(pin, OUTPUT_OPEN_DRAIN);
        break;
    default:
        pinMode(pin, INPUT);
    }
}

/* The scan tick calls these often: on ESP32 skip the Arduino wrappers
 * (gpio_* is little more than a register access). Neither runs in an ISR
 * (see the scan timer below), so no IRAM_ATTR is needed. */
int hal_pin_read(uint16_t pin)
{
#ifdef ESP32
    return gpio_get_level((gpio_num_t)pin);
#else
    return digitalRead(pin);
#endif
}

void hal_pin_write(uint16_t pin, int level)
{
#ifdef ESP32
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
#else
    digitalWrite(pin, level ? HIGH : LOW);
#endif
}

int32_t hal_adc_read(uint16_t pin)
{
#ifdef ESP32
    if (digitalPinToAnalogChannel(pin) < 0)
        return -1;
    return analogRead(pin);                 /* 12 bit, set in gm_esp_begin */
#else
    if (pin != A0)                          /* the ESP8266 has one ADC pin */
        return -1;
    uint32_t v = analogRead(A0);            /* 0..1024 */
    return v >= 1023 ? 4095 : (int32_t)(v * 4095u / 1023u);
#endif
}

/* ---- I2C -------------------------------------------------------------- */

static uint8_t i2c_up;

static void i2c_begin(void)
{
    if (i2c_up)
        return;
#if defined(GM_I2C_SDA) && defined(GM_I2C_SCL)
    Wire.begin(GM_I2C_SDA, GM_I2C_SCL);
#else
    Wire.begin();                           /* board default pins */
#endif
    Wire.setClock(400000);
    i2c_up = 1;
}

int hal_i2c_write(uint8_t addr, const uint8_t *p, uint32_t n)
{
    i2c_begin();
    Wire.beginTransmission(addr);
    Wire.write(p, n);
    return Wire.endTransmission() == 0 ? 0 : -1;
}

int hal_i2c_read(uint8_t addr, uint8_t *p, uint32_t n)
{
    i2c_begin();
    if (Wire.requestFrom(addr, (uint8_t)n) != n)
        return -1;
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)Wire.read();
    return 0;
}

/* ---- identity, reboot ------------------------------------------------- */

/* uid: MAC (6 bytes), 'E' 'S', chip, 0, 0, 0. */
void hal_uid(uint8_t uid[12])
{
    memset(uid, 0, 12);
#ifdef ESP32
    esp_efuse_mac_get_default(uid);
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    uid[8] = 3;
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    uid[8] = 2;
#else
    uid[8] = 1;
#endif
#else
    wifi_get_macaddr(STATION_IF, uid);
    uid[8] = 0x66;                          /* 8266 */
#endif
    uid[6] = 'E';
    uid[7] = 'S';
}

void hal_reboot(int to_bootloader)
{
    (void)to_bootloader;                    /* A/B: no separate update mode */
    hal_midi_flush(50);
    delay(20);
    ESP.restart();
    for (;;)
        delay(1);
}

/* ---- scan timer ------------------------------------------------------- */

static void (*scan_fn)(void);
static uint32_t scan_period;

#ifdef ESP32
/* esp_timer fires in its own task (core 0); it wakes a task pinned to the
 * loop() core, so the tick preempts loop() like an ISR would on the NUC,
 * but runs in task context (no IRAM or ISR restrictions). Missed ticks
 * collapse into one. */
static TaskHandle_t scan_task;

static void scan_timer_cb(void *arg)
{
    (void)arg;
    xTaskNotifyGive(scan_task);
}

static void scan_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t t0 = micros();
        scan_fn();
        gm_diag_scan_us(micros() - t0);     /* diag.scan_max */
    }
}

void gm_esp_scan_start(uint32_t period_us, void (*tick)(void))
{
    scan_fn = tick;
    scan_period = period_us < 100 ? 100 : period_us;
    xTaskCreatePinnedToCore(scan_task_fn, "gm_scan", 3072, NULL,
                            configMAX_PRIORITIES - 4, &scan_task, xPortGetCoreID());
    const esp_timer_create_args_t a = {
        .callback = scan_timer_cb, .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK, .name = "gm_scan",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t t;
    esp_timer_create(&a, &t);
    esp_timer_start_periodic(t, scan_period);
}

void gm_esp_scan_service(void)
{
}
#else
/* ESP8266: an ISR would have to keep the whole scan path in IRAM and stop
 * during every flash write (EEPROM, OTA), so the tick runs from loop()
 * instead, paced with micros(). Jitter = the longest gm_poll() step. */
static uint32_t scan_next;
static uint8_t in_scan;

void gm_esp_scan_start(uint32_t period_us, void (*tick)(void))
{
    scan_period = period_us < 100 ? 100 : period_us;
    scan_next = micros();
    scan_fn = tick;
}

void gm_esp_scan_service(void)
{
    if (!scan_fn || in_scan)
        return;
    uint32_t now = micros();
    if ((int32_t)(now - scan_next) < 0)
        return;
    in_scan = 1;
    scan_fn();
    in_scan = 0;
    gm_diag_scan_us(micros() - now);        /* diag.scan_max */
    scan_next += scan_period;
    if ((int32_t)(now - scan_next) > (int32_t)(4 * scan_period))
        scan_next = now + scan_period;     /* far behind: don't burst */
}
#endif

/* ---- setup / loop ----------------------------------------------------- */

void gm_esp_begin(void)
{
#ifdef ESP32
    analogReadResolution(12);               /* S2 defaults to 13 bit */
#if CONFIG_FREERTOS_UNICORE
    disableCore0WDT();                      /* loop() never blocks */
#endif
#endif
#if defined(GM_I2C_SDA) && defined(GM_I2C_SCL)
    i2c_begin();
#endif
    gm_esp_midi_begin();
}

void gm_esp_loop(void)
{
    /* diag.loop_max: from one call to the next, gm_poll() included */
    static uint32_t t_last;
    uint32_t t = micros();
    if (t_last)
        gm_diag_loop_us(t - t_last);
    t_last = t | 1u;
    hal_midi_poll();
    gm_esp_scan_service();
}

/* ---- why the app started (diag.reset_reason, sysex.md 4.10) ------------ */

uint32_t hal_reset_reason(void)
{
#ifdef ESP32
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return GM_RESET_POWER_ON;
    case ESP_RST_EXT:       return GM_RESET_PIN;
    case ESP_RST_SW:        return GM_RESET_SOFTWARE;
    case ESP_RST_PANIC:     return GM_RESET_CRASH;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return GM_RESET_WATCHDOG;
    case ESP_RST_DEEPSLEEP: return GM_RESET_LOW_POWER;
    case ESP_RST_BROWNOUT:  return GM_RESET_BROWN_OUT;
    default:                return GM_RESET_UNKNOWN;
    }
#else
    const rst_info *r = ESP.getResetInfoPtr();
    switch (r ? r->reason : 0xFFu) {
    case REASON_DEFAULT_RST:      return GM_RESET_POWER_ON;
    case REASON_EXT_SYS_RST:      return GM_RESET_PIN;
    case REASON_SOFT_RESTART:     return GM_RESET_SOFTWARE;
    case REASON_EXCEPTION_RST:    return GM_RESET_CRASH;
    case REASON_WDT_RST:
    case REASON_SOFT_WDT_RST:     return GM_RESET_WATCHDOG;
    case REASON_DEEP_SLEEP_AWAKE: return GM_RESET_LOW_POWER;
    default:                      return GM_RESET_UNKNOWN;
    }
#endif
}

/* ---- LED strips (gm_hal.h hal_ledstrip_*, the ws2812 hub) -------------
 *
 * ESP32 / S2 / S3: by RMT through the Arduino core (esp32-hal-rmt,
 * Arduino-ESP32 3.x). Each strip has an RMT TX channel at a 10 MHz tick
 * (0.1 us) and its own symbol buffer: one symbol a bit, WS2812B / SK6812
 * timings (a 0 is 0.4 us high then 0.8 us low, a 1 is 0.8 us high then
 * 0.5 us low: inside both parts' windows), then one symbol of 300 us low,
 * the latch, so "transmit completed" means the LEDs show the frame.
 * hal_ledstrip_write encodes the bytes and starts rmtWriteAsync: it doesn't
 * wait, and the RMT driver feeds the channel from its interrupt while the
 * main loop goes on. A frame of 16 LEDs is about 0.8 ms on the wire.
 * Here, not in a file of its own: the library is an archive, and the
 * core's weak defaults (vpin.c) would keep a file only these are in from
 * being linked.
 *
 * ESP8266 has no RMT: no strip there (the builder refuses ws2812), and the
 * weak defaults stay. */
#if defined(ESP32) && __has_include("esp32-hal-rmt.h")
#include "esp32-hal-rmt.h"
#endif

#if defined(ESP32) && defined(SOC_RMT_SUPPORTED) && SOC_RMT_SUPPORTED
#include <stdlib.h>

#define LS_MAX 8                    /* strips (ESP32 has 8 RMT TX channels, S2 / S3 4) */
#define LS_TICK_HZ 10000000u        /* 0.1 us */
#define LS_T0H 4u                   /* ticks: a 0 is 0.4 + 0.8 us */
#define LS_T0L 8u
#define LS_T1H 8u                   /* a 1 is 0.8 + 0.5 us */
#define LS_T1L 5u
#define LS_LATCH 1500u              /* x 2: 300 us low */

/* How many of the RMT's memory blocks each strip's channel takes: more
 * means fewer refills from the interrupt, so a late refill (Wi-Fi, flash)
 * is less likely to cut a frame short. One strip can have two. */
#ifndef GM_LEDSTRIPS
#define GM_LEDSTRIPS 2
#endif
#define LS_BLOCKS (GM_LEDSTRIPS > 1 ? RMT_MEM_NUM_BLOCKS_1 : RMT_MEM_NUM_BLOCKS_2)

typedef struct {
    int16_t pin;                    /* -1 = free */
    uint32_t n;                     /* bytes a frame, at most */
    rmt_data_t *sym;                /* n * 8 + 1 symbols */
    uint8_t started;                /* a frame went out */
} strip_t;

static strip_t strips[LS_MAX] = {
    { -1, 0, 0, 0 }, { -1, 0, 0, 0 }, { -1, 0, 0, 0 }, { -1, 0, 0, 0 },
    { -1, 0, 0, 0 }, { -1, 0, 0, 0 }, { -1, 0, 0, 0 }, { -1, 0, 0, 0 },
};

static strip_t *strip_of(uint16_t pin)
{
    for (uint32_t i = 0; i < LS_MAX; i++)
        if (strips[i].pin == (int16_t)pin)
            return &strips[i];
    return 0;
}

extern "C" int hal_ledstrip_open(uint16_t pin, uint32_t n)
{
    strip_t *s = strip_of(pin);
    if (s)
        return n <= s->n;
    for (uint32_t i = 0; i < LS_MAX && !s; i++)
        if (strips[i].pin < 0)
            s = &strips[i];         /* a free slot */
    if (!s || n == 0)
        return 0;
    rmt_data_t *sym = (rmt_data_t *)calloc(n * 8u + 1u, sizeof(rmt_data_t));
    if (!sym)
        return 0;
    if (!rmtInit((int)pin, RMT_TX_MODE, LS_BLOCKS, LS_TICK_HZ)) {
        free(sym);
        return 0;
    }
    rmtSetEOT((int)pin, 0);         /* the line idles low */
    s->pin = (int16_t)pin;
    s->n = n;
    s->sym = sym;
    s->started = 0;
    return 1;
}

extern "C" int hal_ledstrip_busy(uint16_t pin)
{
    strip_t *s = strip_of(pin);
    return s && s->started && !rmtTransmitCompleted((int)pin);
}

extern "C" int hal_ledstrip_write(uint16_t pin, const uint8_t *b, uint32_t n)
{
    strip_t *s = strip_of(pin);
    if (!s || n > s->n || (s->started && !rmtTransmitCompleted((int)pin)))
        return 0;
    rmt_data_t *p = s->sym;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t m = 0x80u; m; m >>= 1) {
            int one = (b[i] & m) != 0;
            p->level0 = 1;
            p->duration0 = one ? LS_T1H : LS_T0H;
            p->level1 = 0;
            p->duration1 = one ? LS_T1L : LS_T0L;
            p++;
        }
    p->level0 = 0;                  /* the latch */
    p->duration0 = LS_LATCH;
    p->level1 = 0;
    p->duration1 = LS_LATCH;
    p++;
    if (!rmtWriteAsync((int)pin, s->sym, (size_t)(p - s->sym)))
        return 0;
    s->started = 1;
    return 1;
}
#endif
