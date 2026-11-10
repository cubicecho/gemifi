# boot-sim: bootloader/stm32f1/main.c's boot decision (the boot-loop guard,
# the update flag, the entry combo, the image check) on the host, with
# simulated flash, registers and SRAM mailbox. See boot_sim.c.
#
#   make test
#   make clean

ROOT   := ../../../..
PLAT   := ../..
BUILD  ?= build
HOSTCC ?= gcc
CFLAGS := -O1 -g -Wall -Wextra -Werror -Wno-unused-function -DGEMIFI_BOOTLOADER \
          -DBOOT_VERSION_MAJOR=0 -DBOOT_VERSION_MINOR=1 -DBOOT_VERSION_PATCH=2 \
          -Istub -I$(ROOT)/bootloader/stm32f1 -I$(PLAT)/include -I$(PLAT)/boards -I$(ROOT)/core/include

# The board profile's generated layout, as in a firmware build (platform/board-profile.mk).
PLATFORM_NAME := stm32f1
BOARD ?= m-audio-code61
include $(ROOT)/platform/board-profile.mk
CFLAGS += -I$(BOARD_GEN)

SRCS := boot_sim.c $(PLAT)/src/crc32.c $(ROOT)/core/src/gsx.c

.PHONY: test clean
test: $(BUILD)/boot_sim
	$(BUILD)/boot_sim

$(BUILD)/boot_sim: $(SRCS) $(ROOT)/bootloader/stm32f1/main.c $(wildcard stub/*.h $(PLAT)/include/*.h) $(BOARD_GEN)/board_layout.h | $(BUILD)
	$(HOSTCC) $(CFLAGS) -o $@ $(SRCS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
