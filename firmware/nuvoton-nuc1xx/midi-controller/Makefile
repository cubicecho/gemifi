# midi-controller (NUC1xx): the generic gemifi MIDI controller firmware.
# The hardware comes from a generated device config (core/README.md).
#
#   make                                  -> Keystation Mini 32 (devices/generated-example)
#   make DEVICE_DIR=<dir> BUILD=<dir>     -> another device (tools/builder does this)
#
# Output: $(BUILD)/midi-controller.gfw, for the bootloader / flash-aprom.sh.

TARGET     := midi-controller
PLAT       := ../../../platform/nuvoton-nuc1xx
CORE       := ../../../core
DEVICE_DIR ?= ../../../devices/generated-example

# board_id for the image header comes from the device config.
MKIMAGE_BOARD := $(shell sed -n 's/^\#define GM_BOARD_ID[ \t]*//p' $(DEVICE_DIR)/gm_device_config.h)

# Firmware version: the device YAML's `version:` (GM_FW_VERSION_STR in the
# device config), else 0.1.0. `make FW_VERSION=x.y.z` overrides it.
DEVICE_FW_VERSION := $(shell sed -n 's/^\#define GM_FW_VERSION_STR[ \t]*"\(.*\)"/\1/p' $(DEVICE_DIR)/gm_device_config.h)
FW_VERSION ?= $(or $(DEVICE_FW_VERSION),0.1.0)

CFLAGS += -I$(DEVICE_DIR)

APP_SRCS := main.c \
            $(PLAT)/src/sys.c \
            $(PLAT)/src/fmc.c \
            $(PLAT)/src/crc32.c \
            $(PLAT)/src/usbd.c \
            $(PLAT)/src/usb_midi.c \
            $(PLAT)/src/hal.c \
            $(PLAT)/src/config_store.c \
            $(CORE)/src/gsx.c \
            $(CORE)/src/params.c \
            $(CORE)/src/app_proto.c \
            $(CORE)/src/settings.c \
            $(CORE)/src/vpin.c \
            $(CORE)/src/controller.c \
            $(DEVICE_DIR)/gm_device_config.c

include $(PLAT)/mk/app.mk

# The device config changes what gets built: rebuild when it changes.
$(BUILD)/flags: $(DEVICE_DIR)/gm_device_config.h
