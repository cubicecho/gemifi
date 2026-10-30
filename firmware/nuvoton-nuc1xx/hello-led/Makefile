# hello-led (M1): blink Oct-, light Oct+ while Oct+ is held.
#
#   make                 -> build/hello-led.bin and build/hello-led.gfw
#   make CROSS=...       -> toolchain not on PATH (see platform/.../mk/rules.mk)
#   make MKIMAGE=...     -> another mkimage command (default tools/host/mkimage.py)
#
# Flash build/hello-led.bin (or .gfw: same bytes, header filled) at 0x0.

TARGET     := hello-led
FW_VERSION ?= 0.1.0
APP_SRCS   := main.c \
              ../../../platform/nuvoton-nuc1xx/src/sys.c

include ../../../platform/nuvoton-nuc1xx/mk/app.mk
