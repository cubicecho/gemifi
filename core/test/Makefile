# Host tests for the core runtime (no hardware, no cross compiler).
#
#   make test    build and run; also checks the firmware's schema blob with
#                the Python parser (tools/host/gemifi_proto.py)
#
# test_controller: the Keystation config (devices/keystation-mini32-mk3.yaml),
#                  with its presets, plus `unlock: true` (the physical unlock
#                  is opt-in; written into the build dir)
# test_controller_wtf: the Keystation as shipped (unlock off, compiled out)
#                  with write_to_flash: { last_preset: true }
# Both use a RAM config store (fake_config_store.c); the flash journals are
# tested per platform.
# test_feedback:   host feedback + Mackie Control (feedback-test.yaml)
# test_tm1628:     TM1628 LED driver hubs (tm1628-test.yaml); run again with
#                  GM_TEST_BSRR=1 for the set/reset store path, and with
#                  GM_TEST_NO_CYCLES=1 too for stores without a cycle counter;
#                  test_tm1628_ack: the same with maudio_pad_leds rx_pin, gamma
#                  and white, and a simulated pad board that answers
#                  (RGB_ACK_TEST)
# test_pads:       velocity pads on shared-select muxes (pads-test.yaml)
# test_matrix_hw:  test_matrix.c with the hardware scan's frames
#                  (matrix-hw-test.yaml)
# test_pads_hw:    test_pads.c with the hardware analog scan's sweeps
#                  (pads-hw-test.yaml)
# test_display:    core/src/display.c alone (display-cfg/, fake pins)
# test_din_port:   5-pin DIN MIDI as its own USB port (din-port-test.yaml)
# test_din_merge:  the same, merged onto the main port (din-merge-test.yaml)
# test_leds:       LED looks, idle: / active: (leds-test.yaml), and
# test_leds_local: the same with feedback off (leds-local-test.yaml)
# test_leds_many:  the same with 132 LEDs (leds-test.yaml + 74HC595 chains)
# test_encoders:   encoder acceleration on fake time (encoders-test.yaml)
# test_i2c:        MCP23017 / PCF8574 hubs on a fake bus (i2c-test.yaml)
# test_msg:        the MIDI message kinds: 14-bit CC, (N)RPN, bank, panic,
#                  MMC, SysEx, per-control channel / range (msg-test.yaml)
# test_modes:      layers, cycles, hold modes and buttons: without a key
#                  matrix (modes-test.yaml)
# test_ws2812:     WS2812 / SK6812 strips through a simulated hal_ledstrip_*:
#                  bytes and order, gamma / white, RGBW, only changed frames,
#                  the rate limit, full-colour looks and step colours
#                  (ws2812-test.yaml)

ROOT  := ../..
BUILD ?= build
CC    ?= gcc
# Device configs, generated from their YAML by tools/builder (only
# rewritten when they change).
KS_YAML := $(ROOT)/devices/keystation-mini32-mk3.yaml
DEVICE := $(BUILD)/keystation-unlock.yaml
DEV    := $(BUILD)/device
FB_DEVICE := feedback-test.yaml
FB_DEV    := $(BUILD)/device-feedback
MX_DEVICE := matrix-test.yaml
MX_DEV    := $(BUILD)/device-matrix
HW_DEVICE := matrix-hw-test.yaml
HW_DEV    := $(BUILD)/device-matrix-hw
TM_DEVICE := tm1628-test.yaml
TM_DEV    := $(BUILD)/device-tm1628
TMA_DEVICE := $(BUILD)/tm1628-ack-test.yaml
TMA_DEV    := $(BUILD)/device-tm1628-ack
LED_DEVICE := leds-test.yaml
LED_DEV    := $(BUILD)/device-leds
LEDL_DEVICE := leds-local-test.yaml
LEDL_DEV    := $(BUILD)/device-leds-local
LEDM_DEVICE := $(BUILD)/leds-many-test.yaml
LEDM_DEV    := $(BUILD)/device-leds-many
PAD_DEVICE := pads-test.yaml
PAD_DEV    := $(BUILD)/device-pads
PADHW_DEVICE := pads-hw-test.yaml
PADHW_DEV    := $(BUILD)/device-pads-hw
DINP_DEVICE := din-port-test.yaml
DINP_DEV    := $(BUILD)/device-din-port
DINM_DEVICE := din-merge-test.yaml
DINM_DEV    := $(BUILD)/device-din-merge
ENC_DEVICE := encoders-test.yaml
ENC_DEV    := $(BUILD)/device-encoders
I2C_DEVICE := i2c-test.yaml
I2C_DEV    := $(BUILD)/device-i2c
MSG_DEVICE := msg-test.yaml
MSG_DEV    := $(BUILD)/device-msg
MODES_DEVICE := modes-test.yaml
MODES_DEV    := $(BUILD)/device-modes
WS_DEVICE := ws2812-test.yaml
WS_DEV    := $(BUILD)/device-ws2812
WTF_DEVICE := $(BUILD)/keystation-write-to-flash.yaml
WTF_DEV    := $(BUILD)/device-wtf
GEN_OUT := $(shell mkdir -p $(BUILD) && { cat $(KS_YAML); echo 'unlock: true'; } > $(DEVICE).new && \
                     { cmp -s $(DEVICE).new $(DEVICE) || mv $(DEVICE).new $(DEVICE); } && rm -f $(DEVICE).new && \
                     python3 $(ROOT)/tools/builder/gemifi_build.py generate $(DEVICE) -o $(DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(FB_DEVICE) -o $(FB_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(FB_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(MX_DEVICE) -o $(MX_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(MX_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(HW_DEVICE) -o $(HW_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(HW_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(TM_DEVICE) -o $(TM_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(TM_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
# The same device with the pad board's answers (maudio_pad_leds rx_pin).
GEN_OUT := $(shell mkdir -p $(BUILD) && sed -e 's/{ id: pads, tx_pin: PA2, count: 4 }/{ id: pads, tx_pin: PA2, rx_pin: PA3, count: 4, gamma: 2.2, white: [255, 200, 220] }/' -e 's/^    grids: 7$$/    grids: 7\n    fade: true/' $(TM_DEVICE) > $(TMA_DEVICE) && grep -q 'rx_pin: PA3' $(TMA_DEVICE) && grep -q 'fade: true' $(TMA_DEVICE) && python3 $(ROOT)/tools/builder/gemifi_build.py generate $(TMA_DEVICE) -o $(TMA_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(TMA_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif

GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(PAD_DEVICE) -o $(PAD_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(PAD_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(PADHW_DEVICE) -o $(PADHW_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(PADHW_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(DINP_DEVICE) -o $(DINP_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(DINP_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(DINM_DEVICE) -o $(DINM_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(DINM_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell mkdir -p $(BUILD) && { cat $(KS_YAML); echo 'write_to_flash: { last_preset: true }'; } > $(WTF_DEVICE).new && \
                     { cmp -s $(WTF_DEVICE).new $(WTF_DEVICE) || mv $(WTF_DEVICE).new $(WTF_DEVICE); } && rm -f $(WTF_DEVICE).new && \
                     python3 $(ROOT)/tools/builder/gemifi_build.py generate $(WTF_DEVICE) -o $(WTF_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(WTF_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(LED_DEVICE) -o $(LED_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(LED_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(LEDL_DEVICE) -o $(LEDL_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(LEDL_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
# leds-test.yaml with 125 more LEDs on 74HC595 chains: 132 in all, so
# n_leds is over 127 (#115: a signed compare there skipped every button LED).
GEN_OUT := $(shell mkdir -p $(BUILD) && { cat $(LED_DEVICE); \
                     python3 -c 'for i in range(125): print(f"  - {{ id: many_{i}, pin: {{ sn74hc595: sr_{i // 32}, number: {i % 32} }} }}")'; \
                     python3 -c 'print("sn74hc595:"); [print(f"  - {{ id: sr_{k}, data_pin: {{ number: PA0, allow_other_uses: true }}, clock_pin: {{ number: PA1, allow_other_uses: true }}, latch_pin: PA{4 + k}, sr_count: 4 }}") for k in range(4)]'; } > $(LEDM_DEVICE).new && \
                     { cmp -s $(LEDM_DEVICE).new $(LEDM_DEVICE) || mv $(LEDM_DEVICE).new $(LEDM_DEVICE); } && rm -f $(LEDM_DEVICE).new && \
                     python3 $(ROOT)/tools/builder/gemifi_build.py generate $(LEDM_DEVICE) -o $(LEDM_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(LEDM_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(ENC_DEVICE) -o $(ENC_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(ENC_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(I2C_DEVICE) -o $(I2C_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(I2C_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(MSG_DEVICE) -o $(MSG_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(MSG_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(MODES_DEVICE) -o $(MODES_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(MODES_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(WS_DEVICE) -o $(WS_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(WS_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror \
          -I$(ROOT)/core/include -I$(ROOT)/platform/nuvoton-nuc1xx/include

CORE_SRCS := $(ROOT)/core/src/gsx.c $(ROOT)/core/src/params.c $(ROOT)/core/src/app_proto.c \
             $(ROOT)/core/src/settings.c $(ROOT)/core/src/vpin.c $(ROOT)/core/src/display.c $(ROOT)/core/src/din.c $(ROOT)/core/src/controller.c \
             $(ROOT)/core/src/midi_in.c $(ROOT)/core/src/mcu.c $(ROOT)/core/src/presets.c $(ROOT)/core/src/safety.c \
             fake_config_store.c

SRCS := test_controller.c $(CORE_SRCS) $(DEV)/gm_device_config.c
WTF_SRCS := test_controller.c $(CORE_SRCS) $(WTF_DEV)/gm_device_config.c

FB_SRCS := test_feedback.c $(CORE_SRCS) $(FB_DEV)/gm_device_config.c

MX_SRCS := test_matrix.c $(CORE_SRCS) $(MX_DEV)/gm_device_config.c
HW_SRCS := test_matrix.c $(CORE_SRCS) $(HW_DEV)/gm_device_config.c
TM_SRCS := test_tm1628.c $(CORE_SRCS) $(TM_DEV)/gm_device_config.c
TMA_SRCS := test_tm1628.c $(CORE_SRCS) $(TMA_DEV)/gm_device_config.c
PAD_SRCS := test_pads.c $(CORE_SRCS) $(PAD_DEV)/gm_device_config.c
PADHW_SRCS := test_pads.c $(CORE_SRCS) $(PADHW_DEV)/gm_device_config.c
DINP_SRCS := test_din.c $(CORE_SRCS) $(DINP_DEV)/gm_device_config.c
DINM_SRCS := test_din.c $(CORE_SRCS) $(DINM_DEV)/gm_device_config.c
LED_SRCS := test_leds.c $(CORE_SRCS) $(LED_DEV)/gm_device_config.c
LEDL_SRCS := test_leds.c $(CORE_SRCS) $(LEDL_DEV)/gm_device_config.c
LEDM_SRCS := test_leds.c $(CORE_SRCS) $(LEDM_DEV)/gm_device_config.c
ENC_SRCS := test_encoders.c $(CORE_SRCS) $(ENC_DEV)/gm_device_config.c
I2C_SRCS := test_i2c.c $(CORE_SRCS) $(I2C_DEV)/gm_device_config.c
MSG_SRCS := test_msg.c $(CORE_SRCS) $(MSG_DEV)/gm_device_config.c
MODES_SRCS := test_modes.c $(CORE_SRCS) $(MODES_DEV)/gm_device_config.c
WS_SRCS := test_ws2812.c $(CORE_SRCS) $(WS_DEV)/gm_device_config.c

.PHONY: test clean
test: $(BUILD)/test_controller $(BUILD)/test_controller_wtf $(BUILD)/test_feedback $(BUILD)/test_matrix $(BUILD)/test_matrix_hw $(BUILD)/test_tm1628 $(BUILD)/test_tm1628_ack $(BUILD)/test_pads $(BUILD)/test_pads_hw $(BUILD)/test_display $(BUILD)/test_din_port $(BUILD)/test_din_merge $(BUILD)/test_leds $(BUILD)/test_leds_local $(BUILD)/test_leds_many $(BUILD)/test_encoders $(BUILD)/test_i2c $(BUILD)/test_msg $(BUILD)/test_modes $(BUILD)/test_ws2812
	$(BUILD)/test_controller $(BUILD)/schema.bin
	GM_TEST_SLOW_COLS=1 $(BUILD)/test_controller $(BUILD)/schema.bin
	GM_TEST_SLOW_ROWS=1 $(BUILD)/test_controller $(BUILD)/schema.bin
	python3 -I check_schema.py $(BUILD)/schema.bin
	$(BUILD)/test_controller_wtf
	$(BUILD)/test_feedback
	$(BUILD)/test_matrix
	GM_TEST_FAST_COLS=1 $(BUILD)/test_matrix
	GM_TEST_FAST_COLS=1 GM_TEST_BSRR=1 $(BUILD)/test_matrix
	$(BUILD)/test_matrix_hw
	$(BUILD)/test_tm1628
	GM_TEST_BSRR=1 $(BUILD)/test_tm1628
	GM_TEST_BSRR=1 GM_TEST_NO_CYCLES=1 $(BUILD)/test_tm1628
	$(BUILD)/test_tm1628_ack
	$(BUILD)/test_pads
	$(BUILD)/test_pads_hw
	$(BUILD)/test_display
	$(BUILD)/test_din_port
	$(BUILD)/test_din_merge
	$(BUILD)/test_leds
	$(BUILD)/test_leds_local
	$(BUILD)/test_leds_many
	$(BUILD)/test_encoders
	$(BUILD)/test_i2c
	$(BUILD)/test_msg
	$(BUILD)/test_modes
	$(BUILD)/test_ws2812

$(BUILD)/test_controller: $(SRCS) $(wildcard $(ROOT)/core/include/*.h) fake_config_store.h $(DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DHAL_APP_FW_UPLOAD=0 -I$(DEV) $(SRCS) -Wl,--wrap=gm_display_message -o $@

$(BUILD)/test_controller_wtf: $(WTF_SRCS) $(wildcard $(ROOT)/core/include/*.h) fake_config_store.h $(WTF_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DHAL_APP_FW_UPLOAD=0 -I$(WTF_DEV) $(WTF_SRCS) -Wl,--wrap=gm_display_message -o $@

$(BUILD)/test_feedback: $(FB_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(FB_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(FB_DEV) $(FB_SRCS) -o $@

$(BUILD)/test_matrix: $(MX_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(MX_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(MX_DEV) $(MX_SRCS) -Wl,--wrap=gm_display_message -o $@

$(BUILD)/test_matrix_hw: $(HW_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(HW_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DHW_SCAN_TEST -I$(HW_DEV) $(HW_SRCS) -Wl,--wrap=gm_display_message -o $@

$(BUILD)/test_tm1628: $(TM_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(TM_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DGM_TEST_STORE -I$(TM_DEV) $(TM_SRCS) -o $@

$(BUILD)/test_tm1628_ack: $(TMA_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(TMA_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DGM_TEST_STORE -DRGB_ACK_TEST -I$(TMA_DEV) $(TMA_SRCS) -o $@

$(BUILD)/test_pads: $(PAD_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(PAD_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(PAD_DEV) $(PAD_SRCS) -o $@

$(BUILD)/test_pads_hw: $(PADHW_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(PADHW_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DHW_ADC_TEST -I$(PADHW_DEV) $(PADHW_SRCS) -o $@

$(BUILD)/test_display: test_display.c $(ROOT)/core/src/display.c $(wildcard $(ROOT)/core/include/*.h) display-cfg/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Idisplay-cfg test_display.c $(ROOT)/core/src/display.c -o $@

$(BUILD)/test_din_port: $(DINP_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(DINP_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(DINP_DEV) $(DINP_SRCS) -o $@

$(BUILD)/test_din_merge: $(DINM_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(DINM_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(DINM_DEV) $(DINM_SRCS) -o $@

$(BUILD)/test_leds: $(LED_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(LED_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(LED_DEV) $(LED_SRCS) -o $@

$(BUILD)/test_leds_local: $(LEDL_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(LEDL_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(LEDL_DEV) $(LEDL_SRCS) -o $@

$(BUILD)/test_leds_many: $(LEDM_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(LEDM_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(LEDM_DEV) $(LEDM_SRCS) -o $@

$(BUILD)/test_encoders: $(ENC_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(ENC_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(ENC_DEV) $(ENC_SRCS) -o $@

clean:
	rm -rf $(BUILD)

$(BUILD)/test_i2c: $(I2C_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(I2C_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(I2C_DEV) $(I2C_SRCS) -o $@

$(BUILD)/test_msg: $(MSG_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(MSG_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(MSG_DEV) $(MSG_SRCS) -o $@

$(BUILD)/test_modes: $(MODES_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(MODES_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(MODES_DEV) $(MODES_SRCS) -Wl,--wrap=gm_display_message -o $@

$(BUILD)/test_ws2812: $(WS_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(WS_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(WS_DEV) $(WS_SRCS) -o $@
