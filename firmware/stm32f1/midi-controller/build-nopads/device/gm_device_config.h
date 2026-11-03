/* gm_device_config: do not edit; generated from /tmp/claude-1000/-home-vantreeseba-code-cubicecho-gemifi/042868f3-3a64-4313-a18f-c7aaef12a119/scratchpad/code61-nopads.yaml
 * by tools/builder/gemifi_build.py (format: core/README.md). */

#ifndef GM_DEVICE_CONFIG_H
#define GM_DEVICE_CONFIG_H

#define GM_DEVICE_NAME "Code 61"   /* friendly_name */
#define GM_NODE_NAME "m-audio-code61"
#define GM_BOARD_ID 0x0002
#define GM_FW_VERSION 0x00020003u
#define GM_FW_VERSION_STR "0.2.3"
#define GM_PLATFORM "stm32f1"
#define GM_USB_VID 0x1209
#define GM_USB_PID 0x0002
#define GM_N_HUBS 9
#define GM_N_BUTTONS 41
#define GM_N_LEDS 0
#define GM_N_ANALOGS 9
#define GM_N_ENCODERS 8
#define GM_HAS_MATRIX 1
#define GM_MATRIX_KEYS 61
#define GM_MATRIX_VELOCITY 1
#define GM_HAS_I2C 0
#define GM_TM1628_RAM 20 /* display RAM bytes of all tm1628 hubs */

/* host feedback / DAW (core/README.md "DAW feedback") */
#define GM_FEEDBACK 1
#define GM_LED_FB 0
#define GM_LED_OWNER 0 /* host_then_local */
#define GM_LED_TIMEOUT 200 /* 2s, 10 ms units */
#define GM_ANALOG_PICKUP 0
#define GM_MCU 0
#define GM_HOST_FB 1 /* DAW-script messages (sysex.md 4.6) */
#define GM_RINGS 1
#define GM_USB_DAW_PORT 0

/* presets (research/core/presets-design.md §10.1) */
#define GM_N_PRESETS 1 /* no presets: in the YAML */
#define GM_PRESETS_REVISION 0 /* presets.revision */
#define GM_N_YAML_PRESETS 1 /* slots 1..N come from presets.list */
#define GM_N_MODES 0 /* modes: (core/README.md "Modes") */

#define GM_N_PADS 0 /* pads: (core/README.md "Pads") */

#endif
