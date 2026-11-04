# Host tests for the core runtime (no hardware, no cross compiler).
#
#   make test    build and run; also checks the firmware's schema blob with
#                the Python parser (tools/host/gemifi_proto.py)
#
# test_controller: the Keystation config (devices/keystation-mini32-mk3.yaml),
#                  with its presets
# test_controller_wtf: the same with write_to_flash: { last_preset: true }
#                  (the YAML plus that line, written into the build dir)
# Both use a RAM config store (fake_config_store.c); the flash journals are
# tested per platform.
# test_feedback:   host feedback + Mackie Control (feedback-test.yaml)
# test_tm1628:     TM1628 LED driver hubs (tm1628-test.yaml); run again with
#                  GM_TEST_BSRR=1 for the set/reset store path, and with
#                  GM_TEST_NO_CYCLES=1 too for stores without a cycle counter
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

ROOT  := ../..
BUILD ?= build
CC    ?= gcc
# Device configs, generated from their YAML by tools/builder (only
# rewritten when they change).
DEVICE := $(ROOT)/devices/keystation-mini32-mk3.yaml
DEV    := $(BUILD)/device
FB_DEVICE := feedback-test.yaml
FB_DEV    := $(BUILD)/device-feedback
MX_DEVICE := matrix-test.yaml
MX_DEV    := $(BUILD)/device-matrix
HW_DEVICE := matrix-hw-test.yaml
HW_DEV    := $(BUILD)/device-matrix-hw
TM_DEVICE := tm1628-test.yaml
TM_DEV    := $(BUILD)/device-tm1628
LED_DEVICE := leds-test.yaml
LED_DEV    := $(BUILD)/device-leds
LEDL_DEVICE := leds-local-test.yaml
LEDL_DEV    := $(BUILD)/device-leds-local
PAD_DEVICE := pads-test.yaml
PAD_DEV    := $(BUILD)/device-pads
PADHW_DEVICE := pads-hw-test.yaml
PADHW_DEV    := $(BUILD)/device-pads-hw
DINP_DEVICE := din-port-test.yaml
DINP_DEV    := $(BUILD)/device-din-port
DINM_DEVICE := din-merge-test.yaml
DINM_DEV    := $(BUILD)/device-din-merge
WTF_DEVICE := $(BUILD)/keystation-write-to-flash.yaml
WTF_DEV    := $(BUILD)/device-wtf
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(DEVICE) -o $(DEV) 2>&1 >/dev/null || echo GEN_FAILED)
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
GEN_OUT := $(shell mkdir -p $(BUILD) && { cat $(DEVICE); echo 'write_to_flash: { last_preset: true }'; } > $(WTF_DEVICE).new && \
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

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror \
          -I$(ROOT)/core/include -I$(ROOT)/platform/nuvoton-nuc1xx/include

CORE_SRCS := $(ROOT)/core/src/gsx.c $(ROOT)/core/src/params.c $(ROOT)/core/src/app_proto.c \
             $(ROOT)/core/src/settings.c $(ROOT)/core/src/vpin.c $(ROOT)/core/src/display.c $(ROOT)/core/src/din.c $(ROOT)/core/src/controller.c \
             $(ROOT)/core/src/midi_in.c $(ROOT)/core/src/mcu.c $(ROOT)/core/src/presets.c \
             fake_config_store.c

SRCS := test_controller.c $(CORE_SRCS) $(DEV)/gm_device_config.c
WTF_SRCS := test_controller.c $(CORE_SRCS) $(WTF_DEV)/gm_device_config.c

FB_SRCS := test_feedback.c $(CORE_SRCS) $(FB_DEV)/gm_device_config.c

MX_SRCS := test_matrix.c $(CORE_SRCS) $(MX_DEV)/gm_device_config.c
HW_SRCS := test_matrix.c $(CORE_SRCS) $(HW_DEV)/gm_device_config.c
TM_SRCS := test_tm1628.c $(CORE_SRCS) $(TM_DEV)/gm_device_config.c
PAD_SRCS := test_pads.c $(CORE_SRCS) $(PAD_DEV)/gm_device_config.c
PADHW_SRCS := test_pads.c $(CORE_SRCS) $(PADHW_DEV)/gm_device_config.c
DINP_SRCS := test_din.c $(CORE_SRCS) $(DINP_DEV)/gm_device_config.c
DINM_SRCS := test_din.c $(CORE_SRCS) $(DINM_DEV)/gm_device_config.c
LED_SRCS := test_leds.c $(CORE_SRCS) $(LED_DEV)/gm_device_config.c
LEDL_SRCS := test_leds.c $(CORE_SRCS) $(LEDL_DEV)/gm_device_config.c

.PHONY: test clean
test: $(BUILD)/test_controller $(BUILD)/test_controller_wtf $(BUILD)/test_feedback $(BUILD)/test_matrix $(BUILD)/test_matrix_hw $(BUILD)/test_tm1628 $(BUILD)/test_pads $(BUILD)/test_pads_hw $(BUILD)/test_display $(BUILD)/test_din_port $(BUILD)/test_din_merge $(BUILD)/test_leds $(BUILD)/test_leds_local
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
	$(BUILD)/test_pads
	$(BUILD)/test_pads_hw
	$(BUILD)/test_display
	$(BUILD)/test_din_port
	$(BUILD)/test_din_merge
	$(BUILD)/test_leds
	$(BUILD)/test_leds_local

$(BUILD)/test_controller: $(SRCS) $(wildcard $(ROOT)/core/include/*.h) fake_config_store.h $(DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DHAL_APP_FW_UPLOAD=0 -I$(DEV) $(SRCS) -Wl,--wrap=gm_display_message -o $@

$(BUILD)/test_controller_wtf: $(WTF_SRCS) $(wildcard $(ROOT)/core/include/*.h) fake_config_store.h $(WTF_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(WTF_DEV) $(WTF_SRCS) -o $@

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

clean:
	rm -rf $(BUILD)
