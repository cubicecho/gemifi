# gemifi bootloader for Nuvoton NUC1xx (LDROM, 4 KB).
#
#   make                    -> build/gemifi-boot.bin            (for LDROM 0x00100000)
#   make TEST_IN_APROM=1    -> build/test-aprom/gemifi-boot-test.bin (for APROM 0x0;
#                              uploads go to data flash, see README.md)
#   make CROSS=/opt/arm-gnu-toolchain/bin/arm-none-eabi-   (toolchain not on PATH)
#   make BOARD=keystation-mini32-mk3                        (default)

PLATFORM_DIR := ../../platform/nuvoton-nuc1xx

TARGET := gemifi-boot
ifeq ($(TEST_IN_APROM),1)
TARGET := gemifi-boot-test
BUILD  := build/test-aprom
DEFS   += -DTEST_IN_APROM=1
endif

DEFS += -DVECTOR_IRQS=0 -DSTARTUP_NO_DATA=1
# usb_midi.c: the one-buffer transmit path (no packet ring or host-absent
# tracking, which don't fit). The bootloader only sends replies, flushed.
DEFS += -DMIDI_TX_PKTS=0
# No case tables: if/else chains are smaller here and drop the libgcc
# __gnu_thumb1_case_* helpers (about 24 B of the 4 KB).
CFLAGS += -fno-jump-tables

LDSCRIPT := $(abspath $(PLATFORM_DIR)/ld/bootloader.ld)
SRCS := main.c \
        $(PLATFORM_DIR)/src/startup.c \
        $(PLATFORM_DIR)/src/sys.c \
        $(PLATFORM_DIR)/src/fmc.c \
        $(PLATFORM_DIR)/src/crc32.c \
        $(PLATFORM_DIR)/../../core/src/gsx.c \
        $(PLATFORM_DIR)/src/usbd.c \
        $(PLATFORM_DIR)/src/usb_midi.c

# No mini_libc.c here: the bootloader never needs memcpy/memset, and LTO
# would otherwise keep the "used" copies (36 bytes of the 4 KB). If a change
# makes GCC emit such a call, the link fails loudly instead.

include $(PLATFORM_DIR)/mk/rules.mk
