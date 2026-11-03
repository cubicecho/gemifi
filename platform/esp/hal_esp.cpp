/*
 * hal_esp.cpp: time, pins, ADC, I2C, identity, reboot and the scan timer
 * (gm_hal.h) for ESP32 and ESP8266.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_esp.h"
#include <Wire.h>
#include "gm_hal.h"

#ifdef ESP32
#include <esp_mac.h>
#include <esp_timer.h>
#include <driver/gpio.h>
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
        scan_fn();
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
    hal_midi_poll();
    gm_esp_scan_service();
}
