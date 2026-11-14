# midi-controller (NUC1xx): the generic gemifi MIDI controller firmware.
# The hardware comes from a generated device config (core/README.md).
#
#   make                                  -> Keystation Mini 32 (devices/keystation-mini32-mk3.yaml)
#   make DEVICE=<device.yaml>             -> another device YAML
#   make DEVICE_DIR=<dir> BUILD=<dir>     -> an already generated config (tools/builder does this)
#
# Output: $(BUILD)/midi-controller.gfw, for the bootloader / flash-aprom.sh.

TARGET     := midi-controller
PLAT       := ../../../platform/nuvoton-nuc1xx
CORE       := ../../../core
DEVICE     ?= ../../../devices/keystation-mini32-mk3.yaml

# Without DEVICE_DIR, generate the device config from the YAML into the
# build dir on every make (fast; files are only rewritten when they change).
ifndef DEVICE_DIR
DEVICE_DIR := $(or $(BUILD),build)/device
GEN_OUT := $(shell python3 ../../../tools/builder/gemifi_build.py generate $(DEVICE) -o $(DEVICE_DIR) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif
endif

# board_id for the image header and the board profile come from the device
# config (the YAML's board_id and board:; the builder also passes BOARD=).
MKIMAGE_BOARD := $(shell sed -n 's/^\#define GM_BOARD_ID[ \t]*//p' $(DEVICE_DIR)/gm_device_config.h)
BOARD_ID := $(MKIMAGE_BOARD)
DEVICE_BOARD := $(shell sed -n 's/^\#define GM_BOARD_PROFILE[ \t]*"\([^"]*\)".*/\1/p' $(DEVICE_DIR)/gm_device_config.h)
ifneq ($(DEVICE_BOARD),)
BOARD ?= $(DEVICE_BOARD)
endif

# Firmware version: the device YAML's `version:` (GM_FW_VERSION_STR in the
# device config), else 0.1.0. `make FW_VERSION=x.y.z` overrides it.
DEVICE_FW_VERSION := $(shell sed -n 's/^\#define GM_FW_VERSION_STR[ \t]*"\(.*\)"/\1/p' $(DEVICE_DIR)/gm_device_config.h)
FW_VERSION ?= $(or $(DEVICE_FW_VERSION),0.1.0)

CFLAGS += -I$(DEVICE_DIR)

# The components the device builds (components/<name>/): gm_components.mk, written with the config.
GM_ROOT := $(CORE)/..
include $(DEVICE_DIR)/gm_components.mk
CFLAGS += $(addprefix -I,$(GM_COMPONENT_INCS))

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
            $(CORE)/src/display.c $(CORE)/src/din.c \
            $(CORE)/src/controller.c \
            $(CORE)/src/midi_in.c \
            $(CORE)/src/mcu.c \
            $(CORE)/src/presets.c \
            $(CORE)/src/safety.c \
            $(DEVICE_DIR)/gm_device_config.c \
            $(GM_COMPONENT_SRCS)

include $(PLAT)/mk/app.mk

# The device config changes what gets built: rebuild when it changes.
$(BUILD)/flags: $(DEVICE_DIR)/gm_device_config.h
