# host-sim: native (host gcc) simulation of the NUC1xx bootloader, the usb-log
# app and the USB-MIDI packing. See README.md.
#
#   make test     build everything into build/ and run all checks
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

.PHONY: test clean hello-led
test: $(BUILD)/libboot.so $(BUILD)/libboot_test.so $(BUILD)/libapp.so $(BUILD)/midi_test hello-led
	$(BUILD)/midi_test
	SIM_BUILD=$(BUILD) $(PYTHON) -I run_sim.py
	SIM_BUILD=$(BUILD) $(PYTHON) -I run_app.py

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

$(BUILD)/midi_test: midi_test.c $(PLAT)/src/usb_midi.c $(ROOT)/core/src/gsx.c | $(BUILD)
	$(HOSTCC) $(filter-out -fPIC,$(CFLAGS)) $^ -o $@

clean:
	rm -rf $(BUILD)
