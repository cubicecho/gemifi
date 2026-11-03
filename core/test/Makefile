# Host tests for the core runtime (no hardware, no cross compiler).
#
#   make test    build and run; also checks the firmware's schema blob with
#                the Python parser (tools/host/gemifi_proto.py)
#
# test_controller: the Keystation config (devices/keystation-mini32-mk3.yaml),
#                  with its presets
# Both use a RAM config store (fake_config_store.c); the flash journals are
# tested per platform.
# test_feedback:   host feedback + Mackie Control (feedback-test.yaml)
# test_tm1628:     TM1628 LED driver hubs (tm1628-test.yaml)
# test_pads:       velocity pads on shared-select muxes (pads-test.yaml)
# test_display:    core/src/display.c alone (display-cfg/, fake pins)

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
TM_DEVICE := tm1628-test.yaml
TM_DEV    := $(BUILD)/device-tm1628
PAD_DEVICE := pads-test.yaml
PAD_DEV    := $(BUILD)/device-pads
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
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(TM_DEVICE) -o $(TM_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(TM_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif

GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(PAD_DEVICE) -o $(PAD_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(PAD_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror \
          -I$(ROOT)/core/include -I$(ROOT)/platform/nuvoton-nuc1xx/include

CORE_SRCS := $(ROOT)/core/src/gsx.c $(ROOT)/core/src/params.c $(ROOT)/core/src/app_proto.c \
             $(ROOT)/core/src/settings.c $(ROOT)/core/src/vpin.c $(ROOT)/core/src/display.c $(ROOT)/core/src/controller.c \
             $(ROOT)/core/src/midi_in.c $(ROOT)/core/src/mcu.c $(ROOT)/core/src/presets.c \
             fake_config_store.c

SRCS := test_controller.c $(CORE_SRCS) $(DEV)/gm_device_config.c

FB_SRCS := test_feedback.c $(CORE_SRCS) $(FB_DEV)/gm_device_config.c

MX_SRCS := test_matrix.c $(CORE_SRCS) $(MX_DEV)/gm_device_config.c
TM_SRCS := test_tm1628.c $(CORE_SRCS) $(TM_DEV)/gm_device_config.c
PAD_SRCS := test_pads.c $(CORE_SRCS) $(PAD_DEV)/gm_device_config.c

.PHONY: test clean
test: $(BUILD)/test_controller $(BUILD)/test_feedback $(BUILD)/test_matrix $(BUILD)/test_tm1628 $(BUILD)/test_pads $(BUILD)/test_display
	$(BUILD)/test_controller $(BUILD)/schema.bin
	GM_TEST_SLOW_COLS=1 $(BUILD)/test_controller $(BUILD)/schema.bin
	python3 -I check_schema.py $(BUILD)/schema.bin
	$(BUILD)/test_feedback
	$(BUILD)/test_matrix
	GM_TEST_FAST_COLS=1 $(BUILD)/test_matrix
	GM_TEST_FAST_COLS=1 GM_TEST_BSRR=1 $(BUILD)/test_matrix
	$(BUILD)/test_tm1628
	$(BUILD)/test_pads
	$(BUILD)/test_display

$(BUILD)/test_controller: $(SRCS) $(wildcard $(ROOT)/core/include/*.h) fake_config_store.h $(DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(DEV) $(SRCS) -o $@

$(BUILD)/test_feedback: $(FB_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(FB_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(FB_DEV) $(FB_SRCS) -o $@

$(BUILD)/test_matrix: $(MX_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(MX_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(MX_DEV) $(MX_SRCS) -o $@

$(BUILD)/test_tm1628: $(TM_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(TM_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(TM_DEV) $(TM_SRCS) -o $@

$(BUILD)/test_pads: $(PAD_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(PAD_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(PAD_DEV) $(PAD_SRCS) -o $@

$(BUILD)/test_display: test_display.c $(ROOT)/core/src/display.c $(wildcard $(ROOT)/core/include/*.h) display-cfg/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Idisplay-cfg test_display.c $(ROOT)/core/src/display.c -o $@

clean:
	rm -rf $(BUILD)
