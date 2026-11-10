# cs-test: the STM32F1 config_store (platform/stm32f1/src/config_store.c) on
# simulated flash, with host gcc: packed records, reads, compactions, tombstones,
# the old two-page layout, old v2 records, full pages, and a power cut at every
# halfword program and erase of a run of writes (and a second one after it).
#
#   make test
#   make clean

ROOT   := ../../../..
PLAT   := ../..
BUILD  ?= build
HOSTCC ?= gcc
CFLAGS := -O1 -g -Wall -Wextra -Werror -DCS_TEST -Istub -I$(PLAT)/include -I$(ROOT)/core/include

# The board profile's generated layout, as in a firmware build (platform/board-profile.mk).
PLATFORM_NAME := stm32f1
BOARD ?= m-audio-code61
include $(ROOT)/platform/board-profile.mk
CFLAGS += -I$(BOARD_GEN)

.PHONY: test clean
test: $(BUILD)/cs_test
	$(BUILD)/cs_test

$(BUILD)/cs_test: cs_test.c $(PLAT)/src/config_store.c $(PLAT)/src/crc32.c stub/flash.h stub/board.h $(BOARD_GEN)/board_layout.h | $(BUILD)
	$(HOSTCC) $(CFLAGS) -o $@ cs_test.c $(PLAT)/src/config_store.c $(PLAT)/src/crc32.c

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
