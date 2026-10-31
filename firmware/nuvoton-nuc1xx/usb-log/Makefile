# usb-log (M2): USB-MIDI app (1209:0002). Each button press/release sends a
# note plus EVENT and LOG SysEx; answers HELLO and the Universal Identity
# Request; REBOOT(1) goes to the bootloader through the SRAM mailbox.
#
#   make                 -> build/usb-log.bin and build/usb-log.gfw
#   make CROSS=...       -> toolchain not on PATH
#
# Flash build/usb-log.gfw at 0x0 (APROM).

TARGET     := usb-log
FW_VERSION ?= 0.1.0
PLAT       := ../../../platform/nuvoton-nuc1xx
APP_SRCS   := main.c \
              $(PLAT)/src/sys.c \
              $(PLAT)/src/fmc.c \
              $(PLAT)/../../core/src/gsx.c \
              $(PLAT)/src/usbd.c \
              $(PLAT)/src/usb_midi.c

include $(PLAT)/mk/app.mk
