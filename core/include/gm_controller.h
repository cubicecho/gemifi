/*
 * gm_controller.h: the generic MIDI controller runtime (core/src).
 *
 * The platform's main():
 *
 *     platform init (clocks, USB/serial MIDI, timer)
 *     gm_init(fw_version, safe_mode);   (loads the stored presets, gm_presets.h)
 *     start a periodic timer calling gm_scan_tick() every gm_device.scan_us
 *     for (;;) { platform polling (USB); gm_poll(); }
 *
 * gm_scan_tick() runs from the timer interrupt (matrix + encoders: timing
 * matters for velocity); everything else runs in gm_poll().
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_CONTROLLER_H
#define GEMIFI_GM_CONTROLLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* safe_mode = boot combo held: ignore the stored profile (defaults only). */
void gm_init(uint32_t fw_version, int safe_mode);
void gm_poll(void);
void gm_scan_tick(void);
/* The scan period actually in use, in us (starts as gm_device.scan_us). A
 * platform that has to slow the scan down (the scan overran its period)
 * sets it, so key velocities stay right. */
extern volatile uint32_t gm_scan_period_us;
/* Longest time, in hal_cycles() units, of each part of a scan: key matrix,
 * switch matrices, encoders. All 0 where hal_cycles() isn't implemented. */
extern volatile uint32_t gm_scan_part_max[3];
/* Longest time, in hal_cycles() units, of each part of gm_poll(): protocol,
 * DIN, presets and I2C hubs; keys/buttons/ramps; analog; pads; encoders;
 * the LED update (every 10 ms); the LED writes (TM1628 bytes, serial LEDs).
 * By index, not run order: keys/buttons (1) run first. Wall time:
 * interrupts that land in a part count too. */
#define GM_POLL_PARTS 7
extern uint32_t gm_poll_part_max[GM_POLL_PARTS];

/* ---- hardware matrix scan (core/README.md "Hardware scan") ------------
 * Where the platform can scan the matrix rows without the CPU (STM32F1: a
 * timer paces DMA transfers that select each row and copy the column port
 * to RAM), it asks for the plan after gm_init(). The plan lists every row of
 * the key matrix and of every switch matrix once: a select word to store in
 * *sel_reg, and one input register holding every column. A frame is one
 * reading of that register per row, in plan order. Once the platform runs
 * it, it calls gm_scan_hw_started() and then hands each batch of finished
 * frames to gm_scan_frames(), from an interrupt below USB (like the scan
 * tick). gm_scan_tick() keeps running for anything else (shift registers,
 * encoders on MCU pins) and leaves the matrices alone.
 *
 * gm_scan_plan() returns NULL when the device has no hardware_scan: or its
 * matrices can't be described this way (it logs why). n_rows is even: an
 * odd row count gets its last row twice, so a frame is whole 32-bit words. */
#define GM_SCAN_MAX_ROWS 32u
typedef struct {
    volatile uint32_t *sel_reg;          /* one store selects a row (BSRR) */
    uint32_t sel_word[GM_SCAN_MAX_ROWS];
    uint32_t n_rows;
    const volatile uint32_t *in_reg;     /* every column is a bit of this  */
    uint32_t in_mask;                    /* the column bits                 */
    uint32_t row_ns;                     /* time per row                    */
    uint32_t settle_ns;                  /* row selected -> columns read    */
    uint32_t precharge;                  /* 1: drive the columns to their idle
                                            level between rows (see README) */
} gm_scan_plan_t;
const gm_scan_plan_t *gm_scan_plan(void);
/* The scan runs: frame_q4 = the frame period in 1/16 us (key velocity). */
void gm_scan_hw_started(uint32_t frame_q4);
/* n frames of plan.n_rows readings each (the low 16 bits of *in_reg);
 * frames must be 4-byte aligned. lost = frames the platform missed before
 * these (an overrun), so key timing stays right. */
void gm_scan_frames(const uint16_t *raw, uint32_t n, uint32_t lost);
/* 1 once gm_scan_hw_started() ran. */
int gm_scan_hw(void);

/* ---- hardware analog scan (core/README.md "Hardware scan", analog:) ----
 * The analog inputs and pads, read without the CPU: the platform steps the
 * muxes' shared select lines (one store of step_word[s] into *sel_reg) and,
 * settle_ns later, converts every pin[] (each mux's common pin, then the
 * analog inputs and pads on ADC pins); a sweep is all n_steps steps. All the
 * muxes used must share the same select lines. The platform hands each
 * finished sweep to gm_adc_sweep() (n_steps * n_pins readings, step-major:
 * v[s * n_pins + k] = pin[k] at step s), from an interrupt below USB.
 * vpin_analog() then returns the latest readings and the pads look at the
 * strongest reading since their last look, so a strike's peak between two
 * main-loop passes isn't missed. gm_adc_plan() returns NULL without
 * hardware_scan.analog or when the inputs don't fit (it logs why). */
#define GM_ADC_MAX_STEPS 16u
#define GM_ADC_MAX_PINS 8u
typedef struct {
    volatile uint32_t *sel_reg;          /* mux select lines (BSRR); NULL: no muxes */
    uint32_t step_word[GM_ADC_MAX_STEPS];
    uint32_t n_steps;                    /* 1 without muxes */
    uint16_t pin[GM_ADC_MAX_PINS];       /* MCU ADC pins, converted at every step */
    uint32_t n_pins;
    uint32_t settle_ns;                  /* select -> first conversion: the muxes' delay */
} gm_adc_plan_t;
const gm_adc_plan_t *gm_adc_plan(void);
void gm_adc_hw_started(void);
void gm_adc_sweep(const uint16_t *v);
int gm_adc_hw(void);

/* Platform -> core: received MIDI (core/src/midi_in.c). A platform calls
 * one of the two for everything it receives:
 *   gm_midi_rx_byte  a MIDI 1.0 byte stream (serial, BLE): one port, cable 0;
 *   gm_midi_rx_usb   one 4-byte USB-MIDI 1.0 event packet (cable << 4 | CIN,
 *                    then 3 bytes), so the cable (port) is kept.
 * SysEx on the main port goes to the gemifi protocol (app_proto.h) as it
 * always did; channel messages and realtime (clock, start/stop) go to the
 * host feedback code (F1) and, on the DAW port, to Mackie Control (gm_mcu.h)
 * when the device has them (core/README.md "DAW feedback"). */
void gm_midi_rx_byte(uint8_t b);
void gm_midi_rx_usb(const uint8_t ev[4]);

/* Buttons held right now (bit = button index), for the boot combo check
 * before gm_init (reads the pins directly). */
uint32_t gm_buttons_now(void);

/* Core internal (src/presets.c, before a preset switch): Note Off for every
 * sounding key and note button, CC 0 for held CC-momentary buttons and
 * raised CC ramps, Mackie Control buttons up, pitch bend back to centre,
 * toggles and analog pick-up reset. Buttons held now stay silent until
 * released. Sends on the current (old) channel. */
void gm_release_all(void);

#ifdef __cplusplus
}
#endif

#endif
