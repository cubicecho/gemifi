# gemifi bootloader for STM32F1 boards (M-Audio Code 61): 0x08000000, 9 KB.
#
#   make                 -> build/gemifi-boot.bin (flash at 0x08000000 over SWD:
#                           tools/swd/stm32f1/code61.sh write-boot build/gemifi-boot.bin)
#   make CROSS=...       -> toolchain not on PATH

TARGET  := gemifi-boot
BOOT_VERSION ?= 0.1.0
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
