# board-profile.mk: the board profile (platform/<platform>/boards/$(BOARD).json, issue #116) -> the
# generated layout in $(BOARD_GEN): board_layout.h, board_app.ld / board_boot.ld, board.mk, layout.json.
#
# Set PLATFORM_NAME, BOARD, BUILD (and BOARD_ID for a DIY board without an id of its own), then include
# this. It runs tools/builder/gemifi_board.py at parse time (files are only rewritten when they change)
# and includes the generated board.mk: BOARD_ID_RESOLVED, BOARD_HEADER_FILE, BOARD_MKIMAGE_FLAGS,
# BOARD_LAYOUT_JSON.
#
# SPDX-License-Identifier: MIT

BOARD_PROFILE_MK_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PYTHON    ?= python3
BOARD_GEN ?= $(BUILD)/board
BOARD_GEN_TOOL := $(abspath $(BOARD_PROFILE_MK_DIR)/../tools/builder/gemifi_board.py)
BOARD_GEN_OUT := $(shell $(PYTHON) $(BOARD_GEN_TOOL) gen $(PLATFORM_NAME) $(BOARD) -o $(BOARD_GEN) \
                   $(if $(BOARD_ID),--board-id $(BOARD_ID)) 2>&1 || echo GEN_FAILED)
ifneq ($(findstring GEN_FAILED,$(BOARD_GEN_OUT)),)
$(error board $(BOARD): $(subst GEN_FAILED,,$(BOARD_GEN_OUT)))
endif
include $(BOARD_GEN)/board.mk
BOARD_GEN_FILES := $(BOARD_GEN)/board_layout.h $(BOARD_GEN)/board_app.ld $(BOARD_GEN)/board_boot.ld \
                   $(BOARD_GEN_TOOL) $(wildcard $(BOARD_PROFILE_MK_DIR)/$(PLATFORM_NAME)/boards/$(BOARD).json)
