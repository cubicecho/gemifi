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

ROOT  := ../..
BUILD ?= build
CC    ?= gcc
# Device configs, generated from their YAML by tools/builder (only
# rewritten when they change).
DEVICE := $(ROOT)/devices/keystation-mini32-mk3.yaml
DEV    := $(BUILD)/device
FB_DEVICE := feedback-test.yaml
FB_DEV    := $(BUILD)/device-feedback
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(DEVICE) -o $(DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(FB_DEVICE) -o $(FB_DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(FB_DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror \
          -I$(ROOT)/core/include -I$(ROOT)/platform/nuvoton-nuc1xx/include

CORE_SRCS := $(ROOT)/core/src/gsx.c $(ROOT)/core/src/params.c $(ROOT)/core/src/app_proto.c \
             $(ROOT)/core/src/settings.c $(ROOT)/core/src/vpin.c $(ROOT)/core/src/controller.c \
             $(ROOT)/core/src/midi_in.c $(ROOT)/core/src/mcu.c $(ROOT)/core/src/presets.c \
             fake_config_store.c

SRCS := test_controller.c $(CORE_SRCS) $(DEV)/gm_device_config.c

FB_SRCS := test_feedback.c $(CORE_SRCS) $(FB_DEV)/gm_device_config.c

.PHONY: test clean
test: $(BUILD)/test_controller $(BUILD)/test_feedback
	$(BUILD)/test_controller $(BUILD)/schema.bin
	GM_TEST_SLOW_COLS=1 $(BUILD)/test_controller $(BUILD)/schema.bin
	python3 -I check_schema.py $(BUILD)/schema.bin
	$(BUILD)/test_feedback

$(BUILD)/test_controller: $(SRCS) $(wildcard $(ROOT)/core/include/*.h) fake_config_store.h $(DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(DEV) $(SRCS) -o $@

$(BUILD)/test_feedback: $(FB_SRCS) $(wildcard $(ROOT)/core/include/*.h) $(FB_DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(FB_DEV) $(FB_SRCS) -o $@

clean:
	rm -rf $(BUILD)
