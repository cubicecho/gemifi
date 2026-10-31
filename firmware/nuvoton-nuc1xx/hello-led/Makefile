# hello-led (M1): blink Oct-, light Oct+ while Oct+ is held.
#
#   make                 -> build/hello-led.bin and build/hello-led.gfw
#   make CROSS=...       -> toolchain not on PATH (see platform/.../mk/rules.mk)
#   make MKIMAGE=...     -> another mkimage command (default tools/host/mkimage.py)
#
# Flash build/hello-led.bin (or .gfw: same bytes, header filled) at 0x0.
#
# Boot-loop test app (bootloader-design.md 3.6, issue #1):
#   make RESET_AFTER_MS=2000 BUILD=build/bootloop
#     -> build/bootloop/hello-led.gfw resets itself through the bootloader
#        (CPU reset, BS = 1) after about 2 s, without clearing boot_count.
#        The gemifi bootloader should start it 3 times, then stay in
#        update mode.

TARGET     := hello-led
FW_VERSION ?= 0.1.0
ifneq ($(RESET_AFTER_MS),)
DEFS += -DRESET_AFTER_MS=$(RESET_AFTER_MS)
endif
APP_SRCS   := main.c \
              ../../../platform/nuvoton-nuc1xx/src/sys.c

include ../../../platform/nuvoton-nuc1xx/mk/app.mk
