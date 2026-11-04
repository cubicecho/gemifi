# app.mk: rules for an STM32F1 application (linked at 0x08002400, behind
# the vendor bootloader) with a .gfw header at app offset 0x130.
#
# Set TARGET, APP_SRCS and FW_VERSION (maj.min.patch), then include this.
# Produces build/$(TARGET).bin (raw, header unfilled) and
# build/$(TARGET).gfw (header filled by tools/host/mkimage.py).
#
# The .gfw is not what the vendor bootloader takes: the host packer turns
# it into the vendor record stream and writes the whole-image checksum at
# 0x0801B3FE (research/devices/m-audio-code61/analysis/custom-firmware-path.md).
#
# SPDX-License-Identifier: MIT

APP_MK_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PLATFORM_DIR := $(abspath $(APP_MK_DIR)/..)

FW_VERSION ?= 0.1.0
FW_V := $(subst ., ,$(FW_VERSION))
DEFS += -DFW_VERSION_MAJOR=$(word 1,$(FW_V)) \
        -DFW_VERSION_MINOR=$(word 2,$(FW_V)) \
        -DFW_VERSION_PATCH=$(word 3,$(FW_V))
# The bootloader takes uploads (hal_fw_ops() is NULL): leave the app's FW_*
# handling out (core/include/gm_hal.h).
DEFS += -DHAL_APP_FW_UPLOAD=0

# Board id passed to mkimage; must match BOARD_ID in the board header.
MKIMAGE_BOARD ?= 0x0002
PYTHON  ?= python3
MKIMAGE ?= $(PYTHON) $(abspath $(PLATFORM_DIR)/../../tools/host/mkimage.py)
# Header at 0x130 after the 60-IRQ vector table; image base 0x08002400;
# app area 0x08002400..0x0801B3FD (102,398 bytes; the linker already keeps
# the image below 0x0801B000).
MKIMAGE_FLAGS ?= --hdr-offset 0x130 --base 0x08002400 --max-size 102398

LDSCRIPT := $(PLATFORM_DIR)/ld/app.ld
SRCS := $(APP_SRCS) \
        $(PLATFORM_DIR)/src/startup.c \
        $(PLATFORM_DIR)/src/recovery.c \
        $(PLATFORM_DIR)/src/app_header.c \
        $(PLATFORM_DIR)/src/mini_libc.c

EXTRA_ALL += $(BUILD)/$(TARGET).gfw

include $(APP_MK_DIR)/rules.mk

$(BUILD)/$(TARGET).gfw: $(BUILD)/$(TARGET).bin
	$(MKIMAGE) $< $@ --board $(MKIMAGE_BOARD) --version $(FW_VERSION) $(MKIMAGE_FLAGS)
