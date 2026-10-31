# Host tests for the core runtime (no hardware, no cross compiler).
#
#   make test    build and run; also checks the firmware's schema blob with
#                the Python parser (tools/host/gemifi_proto.py)

ROOT  := ../..
BUILD ?= build
CC    ?= gcc
# The Keystation config, generated from its YAML by tools/builder (only
# rewritten when it changes).
DEVICE := $(ROOT)/devices/keystation-mini32-mk3.yaml
DEV    := $(BUILD)/device
GEN_OUT := $(shell python3 $(ROOT)/tools/builder/gemifi_build.py generate $(DEVICE) -o $(DEV) 2>&1 >/dev/null || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(GEN_OUT)),)
$(error $(DEVICE): $(subst GEN_FAILED,,$(GEN_OUT)))
endif

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror \
          -I$(ROOT)/core/include -I$(ROOT)/platform/nuvoton-nuc1xx/include -I$(DEV)

SRCS := test_controller.c \
        $(ROOT)/core/src/gsx.c $(ROOT)/core/src/params.c $(ROOT)/core/src/app_proto.c \
        $(ROOT)/core/src/settings.c $(ROOT)/core/src/vpin.c $(ROOT)/core/src/controller.c \
        $(ROOT)/platform/nuvoton-nuc1xx/src/config_store.c \
        $(ROOT)/platform/nuvoton-nuc1xx/src/crc32.c \
        $(DEV)/gm_device_config.c

.PHONY: test clean
test: $(BUILD)/test_controller
	$(BUILD)/test_controller $(BUILD)/schema.bin
	python3 -I check_schema.py $(BUILD)/schema.bin

$(BUILD)/test_controller: $(SRCS) $(wildcard $(ROOT)/core/include/*.h) $(DEV)/gm_device_config.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) -o $@

clean:
	rm -rf $(BUILD)
