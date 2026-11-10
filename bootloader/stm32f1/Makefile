# gemifi bootloader for STM32F1 boards: 0x08000000, the board profile's
# boot_size (9 KB on the Code 61).
#
#   make                 -> build/gemifi-boot.bin for the Code 61 (flash at 0x08000000 over SWD:
#                           tools/swd/stm32f1/code61.sh write-boot build/gemifi-boot.bin)
#   make BOARD=generic-stm32f103c8 BOARD_ID=0x3F03
#                        -> another board profile (platform/stm32f1/boards/*.json); a DIY
#                           board takes the device YAML's board_id
#   make CROSS=...       -> toolchain not on PATH

TARGET  := gemifi-boot
BOOT_VERSION ?= 0.1.2
PLAT    := ../../platform/stm32f1
CORE    := ../../core

BOOT_V  := $(subst ., ,$(BOOT_VERSION))
DEFS    += -DGEMIFI_BOOTLOADER \
           -DBOOT_VERSION_MAJOR=$(word 1,$(BOOT_V)) \
           -DBOOT_VERSION_MINOR=$(word 2,$(BOOT_V)) \
           -DBOOT_VERSION_PATCH=$(word 3,$(BOOT_V))

LDSCRIPT := $(PLAT)/ld/boot.ld
SRCS := main.c \
        $(PLAT)/src/startup.c \
        $(PLAT)/src/sys.c \
        $(PLAT)/src/flash.c \
        $(PLAT)/src/crc32.c \
        $(PLAT)/src/recovery.c \
        $(PLAT)/src/usbd.c \
        $(PLAT)/src/usb_midi.c \
        $(PLAT)/src/mini_libc.c \
        $(CORE)/src/gsx.c

include $(PLAT)/mk/rules.mk
