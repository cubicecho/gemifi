# probe (STM32F1): first non-stock image for the M-Audio Code 61
# (custom-firmware-path.md 8, step 6). USB-MIDI 1209:0002; reports chip ID,
# flash size, UID, option bytes, reset flags and RAM size as gemifi LOG
# messages every 2 s; answers HELLO, Identity Request and REBOOT.
#
#   make                            -> build/probe.bin and build/probe.gfw
#   make PROBE_DUMP_BOOTLOADER=1    -> also stream 0x08000000-0x080023FF as
#                                      hex LOGs (private; never commit it)
#   make PROBE_RAM=0                -> skip the SRAM size probe
#   make CROSS=...                  -> toolchain not on PATH
#
# Install: pack build/probe.gfw into the vendor update stream (host packer,
# which also writes the image checksum at 0x0801B3FE) and send it to the
# vendor bootloader. The image is linked at 0x08002400.

TARGET     := probe
FW_VERSION ?= 0.1.4
PLAT       := ../../../platform/stm32f1
CORE       := ../../../core

PROBE_DUMP_BOOTLOADER ?= 0
PROBE_RAM  ?= 1
DEFS       += -DPROBE_DUMP_BOOTLOADER=$(PROBE_DUMP_BOOTLOADER) -DPROBE_RAM=$(PROBE_RAM)

APP_SRCS   := main.c \
              $(PLAT)/src/sys.c \
              $(PLAT)/src/flash.c \
              $(PLAT)/src/hal.c \
              $(PLAT)/src/usbd.c \
              $(PLAT)/src/usb_midi.c \
              $(CORE)/src/gsx.c

include $(PLAT)/mk/app.mk
