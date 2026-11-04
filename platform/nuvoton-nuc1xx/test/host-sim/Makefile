# host-sim: native (host gcc) simulation of the NUC1xx bootloader, the usb-log
# app, the USB-MIDI packing, the config_store journal and the midi-controller's
# presets end to end. See README.md.
#
#   make test       build everything into build/ and run all checks
#   make cs-test    only the config_store journal test (no arm-none-eabi-gcc)
#   make presets-test  only the presets end-to-end test (no arm-none-eabi-gcc)
#   make clean
#
# Needs: host gcc, python3, and arm-none-eabi-gcc (to build hello-led.gfw,
# the image the upload tests flash into the simulated APROM).

ROOT   := ../../../..
PLAT   := ../..
BUILD  ?= build
HOSTCC ?= gcc
PYTHON ?= python3
HELLO  := $(ROOT)/firmware/nuvoton-nuc1xx/hello-led

CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fPIC \
          -Istub -I$(PLAT)/include -I$(ROOT)/core/include -DBOARD_HEADER='"simboard.h"'

.PHONY: test clean hello-led cs-test presets-test
MIDI_TESTS := $(BUILD)/midi_test $(BUILD)/midi_test4 $(BUILD)/midi_test0

test: $(BUILD)/libboot.so $(BUILD)/libboot_test.so $(BUILD)/libapp.so $(BUILD)/libctl.so $(MIDI_TESTS) $(BUILD)/cs_test hello-led
	$(BUILD)/midi_test
	$(BUILD)/midi_test4
	$(BUILD)/midi_test0
	$(BUILD)/cs_test $(CS_ARGS)
	SIM_BUILD=$(BUILD) $(PYTHON) -I run_sim.py
	SIM_BUILD=$(BUILD) $(PYTHON) -I run_app.py
	SIM_BUILD=$(BUILD) $(PYTHON) -I run_presets.py

# Only the presets end-to-end test (host gcc only).
presets-test: $(BUILD)/libctl.so
	SIM_BUILD=$(BUILD) $(PYTHON) -I run_presets.py

hello-led:
	$(MAKE) -C $(HELLO)

$(BUILD):
	mkdir -p $@

BOOT_SRCS := harness.c $(ROOT)/core/src/gsx.c $(PLAT)/src/crc32.c
BOOT_DEPS := $(BOOT_SRCS) $(ROOT)/bootloader/nuvoton-nuc1xx/main.c $(wildcard stub/*.h $(PLAT)/include/*.h)

$(BUILD)/libboot.so: $(BOOT_DEPS) | $(BUILD)
	$(HOSTCC) $(CFLAGS) -shared -I$(ROOT)/bootloader/nuvoton-nuc1xx $(BOOT_SRCS) -o $@

$(BUILD)/libboot_test.so: $(BOOT_DEPS) | $(BUILD)
	$(HOSTCC) $(CFLAGS) -shared -DTEST_IN_APROM=1 -I$(ROOT)/bootloader/nuvoton-nuc1xx $(BOOT_SRCS) -o $@

APP_SRCS := harness_app.c $(ROOT)/core/src/gsx.c
$(BUILD)/libapp.so: $(APP_SRCS) $(ROOT)/firmware/nuvoton-nuc1xx/usb-log/main.c $(wildcard stub/*.h $(PLAT)/include/*.h) | $(BUILD)
	$(HOSTCC) $(CFLAGS) -shared -DFW_VERSION_MAJOR=0 -DFW_VERSION_MINOR=1 -DFW_VERSION_PATCH=0 \
	    -I$(ROOT)/firmware/nuvoton-nuc1xx/usb-log $(APP_SRCS) -o $@

# usb_midi.c with its transmit ring as on the NUC (2 packets), as on the
# STM32F1 (4), and the bootloader's one buffer (0).
MIDI_SRCS := midi_test.c $(PLAT)/src/usb_midi.c $(ROOT)/core/src/gsx.c
$(BUILD)/midi_test: $(MIDI_SRCS) | $(BUILD)
	$(HOSTCC) $(filter-out -fPIC,$(CFLAGS)) -DMIDI_TX_PKTS=2 $^ -o $@
$(BUILD)/midi_test4: $(MIDI_SRCS) | $(BUILD)
	$(HOSTCC) $(filter-out -fPIC,$(CFLAGS)) -DMIDI_TX_PKTS=4 $^ -o $@
$(BUILD)/midi_test0: $(MIDI_SRCS) | $(BUILD)
	$(HOSTCC) $(filter-out -fPIC,$(CFLAGS)) -DMIDI_TX_PKTS=0 $^ -o $@

# The midi-controller's protocol side (core/src, presets included) with the
# generated Keystation device config and the real config_store.c. The YAML
# gets write_to_flash: { last_preset: true }, so the device stores the active
# preset on its own and reboots into it (run_presets.py relies on that).
YAML    := $(ROOT)/devices/keystation-mini32-mk3.yaml
DEVICE  := $(BUILD)/keystation-write-to-flash.yaml
DEV_DIR := $(BUILD)/device
CTL_SRCS := harness_ctl.c $(PLAT)/src/config_store.c $(PLAT)/src/crc32.c \
            $(addprefix $(ROOT)/core/src/,gsx.c params.c app_proto.c settings.c controller.c vpin.c display.c din.c midi_in.c mcu.c presets.c) \
            $(DEV_DIR)/gm_device_config.c

$(DEVICE): $(YAML) | $(BUILD)
	{ cat $(YAML); echo 'write_to_flash: { last_preset: true }'; } > $@

$(DEV_DIR)/gm_device_config.c: $(DEVICE) $(wildcard $(ROOT)/tools/builder/*.py) | $(BUILD)
	$(PYTHON) $(ROOT)/tools/builder/gemifi_build.py generate $(DEVICE) -o $(DEV_DIR) >/dev/null

$(BUILD)/libctl.so: $(CTL_SRCS) $(wildcard $(ROOT)/core/include/*.h $(PLAT)/include/*.h) | $(BUILD)
	$(HOSTCC) $(CFLAGS) -shared -DHAL_APP_FW_UPLOAD=0 -I$(DEV_DIR) $(CTL_SRCS) -o $@

# CS_ARGS=-q: fewer power-cut sweeps; -long: 20x the writes, no sweeps.
cs-test: $(BUILD)/cs_test
	$(BUILD)/cs_test $(CS_ARGS)

$(BUILD)/cs_test: cs_test.c $(PLAT)/src/config_store.c $(PLAT)/src/crc32.c $(ROOT)/core/include/config_store.h | $(BUILD)
	$(HOSTCC) -O2 $(filter-out -fPIC -O1,$(CFLAGS)) $(filter %.c,$^) -o $@

clean:
	rm -rf $(BUILD)
