# rules.mk: shared build rules for gemifi STM32F1 targets.
#
# A project Makefile sets TARGET, SRCS (absolute or relative .c paths),
# LDSCRIPT and optionally CFLAGS/DEFS, then includes this file.
#
# Toolchain: arm-none-eabi-gcc from PATH by default. Override with
#   make CROSS=/path/to/arm-gnu-toolchain/bin/arm-none-eabi-
#
# SPDX-License-Identifier: MIT

PLATFORM := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
ROOT     := $(abspath $(PLATFORM)/../..)

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size

BOARD   ?= m-audio-code61
BUILD   ?= build
# The board profile (platform/stm32f1/boards/$(BOARD).json) -> $(BOARD_GEN): the layout
# header and linker include every build uses (platform/board-profile.mk).
PLATFORM_NAME := stm32f1
include $(PLATFORM)/../board-profile.mk

ARCH    := -mcpu=cortex-m3 -mthumb
OPT     ?= -Os
CFLAGS  += $(ARCH) $(OPT) -flto -ffunction-sections -fdata-sections \
           -ffreestanding -fno-tree-loop-distribute-patterns -fno-common \
           -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter \
           -I$(PLATFORM)/include -I$(ROOT)/core/include -I$(PLATFORM)/boards -I$(BOARD_GEN) \
           -DBOARD_HEADER=\"$(BOARD_HEADER_FILE)\" $(DEFS)
LDFLAGS += $(ARCH) $(OPT) -flto -Wl,--gc-sections -nostartfiles -nostdlib \
           -Wl,--no-warn-rwx-segments \
           -L$(BOARD_GEN) -T$(LDSCRIPT) -Wl,-Map=$(BUILD)/$(TARGET).map
LDLIBS  += -lgcc

OBJS := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
vpath %.c $(sort $(dir $(SRCS)))

.PHONY: all clean size
all: $(BUILD)/$(TARGET).bin $(EXTRA_ALL)

$(BUILD):
	mkdir -p $@

# Rebuild everything when the flags change.
$(BUILD)/flags: FORCE | $(BUILD)
	@echo '$(CC) $(CFLAGS) $(LDFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS) $(LDFLAGS)' > $@
FORCE:

$(BUILD)/%.o: %.c $(BUILD)/flags | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT) $(BOARD_GEN_FILES)
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "$(TARGET).bin: $$(wc -c < $@) bytes"

$(BUILD)/$(TARGET).lst: $(BUILD)/$(TARGET).elf
	$(OBJDUMP) -d -S $< > $@

size: $(BUILD)/$(TARGET).elf
	$(SIZE) -A $<

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
