/*
 * hal_esp.c: time, pins, ADC, I2C, identity, reboot and the scan timer
 * (gm_hal.h) on ESP-IDF, for the ESP32 / S2 / S3.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "gm_esp.h"
#include "gm_hal.h"
#include "gm_safety.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"

/* ---- time ------------------------------------------------------------- */

uint32_t hal_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t gm_esp_micros(void)
{
    return (uint32_t)esp_timer_get_time();
}

void hal_delay_us(uint32_t us)
{
    if (us >= 10000) {            /* long waits yield (watchdog, USB, BLE) */
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
        us %= 1000;
    }
    if (us)
        esp_rom_delay_us(us);
}

/* ---- pins ------------------------------------------------------------- */

void hal_pin_mode(uint16_t pin, uint8_t mode)
{
    if (!GPIO_IS_VALID_GPIO(pin))
        return;
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    switch (mode) {
    case HAL_IN_PULLUP:
        c.pull_up_en = GPIO_PULLUP_ENABLE;
        break;
    case HAL_IN_PULLDOWN:
        c.pull_down_en = GPIO_PULLDOWN_ENABLE;
        break;
    case HAL_OUT:                           /* readable too, as Arduino's OUTPUT */
        c.mode = GPIO_MODE_INPUT_OUTPUT;
        break;
    case HAL_OUT_OD:
        c.mode = GPIO_MODE_INPUT_OUTPUT_OD;
        break;
    default:
        break;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) { /* ESP32 GPIO34-39: input only, no pulls */
        c.mode = GPIO_MODE_INPUT;
        c.pull_up_en = GPIO_PULLUP_DISABLE;
        c.pull_down_en = GPIO_PULLDOWN_DISABLE;
    }
    gpio_config(&c);
}

/* The scan tick calls these often: gpio_* is little more than a register
 * access. Neither runs in an ISR (see the scan timer below). */
int hal_pin_read(uint16_t pin)
{
    return gpio_get_level((gpio_num_t)pin);
}

void hal_pin_write(uint16_t pin, int level)
{
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

/* ADC: one-shot reads, 12 dB attenuation (0..~3.1 V), raw 12-bit values as
 * the Arduino builds gave (the S2's 13 bits are shifted down). Each unit
 * and channel is set up on its first read. */
static adc_oneshot_unit_handle_t adc_unit[SOC_ADC_PERIPH_NUM];
static uint64_t adc_ready;                  /* bit = GPIO set up */

int32_t hal_adc_read(uint16_t pin)
{
    adc_unit_t u;
    adc_channel_t ch;
    if (pin >= 64 || adc_oneshot_io_to_channel(pin, &u, &ch) != ESP_OK || u >= SOC_ADC_PERIPH_NUM)
        return -1;
    if (!adc_unit[u]) {
        adc_oneshot_unit_init_cfg_t c = { .unit_id = u, .ulp_mode = ADC_ULP_MODE_DISABLE };
        if (adc_oneshot_new_unit(&c, &adc_unit[u]) != ESP_OK)
            return -1;
    }
    if (!((adc_ready >> pin) & 1u)) {
        adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
        if (adc_oneshot_config_channel(adc_unit[u], ch, &c) != ESP_OK)
            return -1;
        adc_ready |= 1ULL << pin;
    }
    int raw;
    if (adc_oneshot_read(adc_unit[u], ch, &raw) != ESP_OK)
        return -1;
#if SOC_ADC_RTC_MAX_BITWIDTH > 12
    raw >>= SOC_ADC_RTC_MAX_BITWIDTH - 12;
#endif
    return raw;
}

/* ---- I2C -------------------------------------------------------------- */

/* The I2C master driver at 400 kHz, a device handle per address (made on
 * its first use). Blocking: a write returns when it is on the wire. */
#define I2C_DEVS 8
#define I2C_TIMEOUT_MS 20

static i2c_master_bus_handle_t i2c_bus;
static struct {
    uint8_t addr;
    i2c_master_dev_handle_t h;
} i2c_dev[I2C_DEVS];
static uint8_t n_i2c_dev, i2c_failed;

static int i2c_begin(void)
{
    if (i2c_bus)
        return 0;
#if defined(GM_I2C_SDA) && defined(GM_I2C_SCL)
    if (i2c_failed)
        return -1;
    i2c_master_bus_config_t c = {
        .i2c_port = -1,                     /* any free controller */
        .sda_io_num = GM_I2C_SDA,
        .scl_io_num = GM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&c, &i2c_bus) != ESP_OK) {
        i2c_failed = 1;
        return -1;
    }
    return 0;
#else
    return -1;                              /* no i2c: pins in the device YAML */
#endif
}

static i2c_master_dev_handle_t i2c_dev_of(uint8_t addr)
{
    if (i2c_begin())
        return NULL;
    for (uint32_t i = 0; i < n_i2c_dev; i++)
        if (i2c_dev[i].addr == addr)
            return i2c_dev[i].h;
    if (n_i2c_dev >= I2C_DEVS)
        return NULL;
    i2c_device_config_t c = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t h;
    if (i2c_master_bus_add_device(i2c_bus, &c, &h) != ESP_OK)
        return NULL;
    i2c_dev[n_i2c_dev].addr = addr;
    i2c_dev[n_i2c_dev++].h = h;
    return h;
}

int hal_i2c_write(uint8_t addr, const uint8_t *p, uint32_t n)
{
    i2c_master_dev_handle_t h = i2c_dev_of(addr);
    return h && i2c_master_transmit(h, p, n, I2C_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

int hal_i2c_read(uint8_t addr, uint8_t *p, uint32_t n)
{
    i2c_master_dev_handle_t h = i2c_dev_of(addr);
    return h && i2c_master_receive(h, p, n, I2C_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

/* ---- identity, reboot ------------------------------------------------- */

/* uid: MAC (6 bytes), 'E' 'S', chip, 0, 0, 0. */
void hal_uid(uint8_t uid[12])
{
    memset(uid, 0, 12);
    esp_efuse_mac_get_default(uid);
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    uid[8] = 3;
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    uid[8] = 2;
#else
    uid[8] = 1;
#endif
    uid[6] = 'E';
    uid[7] = 'S';
}

void hal_reboot(int to_bootloader)
{
    (void)to_bootloader;                    /* A/B: no separate update mode */
    hal_midi_flush(50);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_restart();
}

/* ---- scan timer ------------------------------------------------------- */

/* esp_timer fires in its own task (core 0); it wakes a task pinned to the
 * main loop's core, so the tick preempts the loop as an ISR would on the
 * NUC, but runs in task context (no IRAM or ISR restrictions). Missed
 * ticks collapse into one. */
static void (*scan_fn)(void);
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
        uint32_t t0 = gm_esp_micros();
        scan_fn();
        gm_diag_scan_us(gm_esp_micros() - t0);   /* diag.scan_max */
    }
}

void gm_esp_scan_start(uint32_t period_us, void (*tick)(void))
{
    scan_fn = tick;
    xTaskCreatePinnedToCore(scan_task_fn, "gm_scan", 3072, NULL,
                            configMAX_PRIORITIES - 4, &scan_task, xPortGetCoreID());
    const esp_timer_create_args_t a = {
        .callback = scan_timer_cb, .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK, .name = "gm_scan",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t t;
    esp_timer_create(&a, &t);
    esp_timer_start_periodic(t, period_us < 100 ? 100 : period_us);
}

/* ---- start / main loop ------------------------------------------------ */

/* diag.scan_max / loop_max are the core's (safety.c); these weak ones stand
 * in for them in a firmware without the core (hello). */
__attribute__((weak)) volatile uint32_t gm_diag_scan_max, gm_diag_loop_max;

void gm_esp_begin(void)
{
    /* NVS: the stored settings, and the radio's calibration. Only a
     * partition that can't be used as it is gets erased (a new NVS format,
     * or no free page). */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
#if defined(GM_I2C_SDA) && defined(GM_I2C_SCL)
    i2c_begin();
#endif
    gm_esp_midi_begin();
}

void gm_esp_loop(void)
{
    /* diag.loop_max: from one call to the next, gm_poll() included */
    static uint32_t t_last;
    uint32_t t = gm_esp_micros();
    if (t_last)
        gm_diag_loop_us(t - t_last);
    t_last = t | 1u;
    hal_midi_poll();
}

/* ---- why the app started (diag.reset_reason, sysex.md 4.10) ------------ */

uint32_t hal_reset_reason(void)
{
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
}
