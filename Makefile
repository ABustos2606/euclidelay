# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Alejandro Bustos

PREFIX ?= /usr/local
LV2DIR ?= $(PREFIX)/lib/lv2
DESTDIR ?=
OPT ?= -O2
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
SAN ?= 0
BUILD_DIR ?= build
ifeq ($(SAN),1)
override BUILD_DIR := $(BUILD_DIR)/san
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
endif

BUNDLE := $(BUILD_DIR)/euclidelay.lv2
SOURCES := src/lv2_entry.cpp src/engine.cpp src/voice.cpp
OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/obj/%.o,$(SOURCES))
METADATA := manifest.ttl euclidelay.ttl
TTL_TARGETS := $(addprefix $(BUNDLE)/,$(METADATA))
TESTS := $(BUILD_DIR)/test_pattern $(BUILD_DIR)/test_voice $(BUILD_DIR)/test_voices $(BUILD_DIR)/test_engine \
	$(BUILD_DIR)/test_changes $(BUILD_DIR)/test_lv2 $(BUILD_DIR)/test_timing
ENGINE_OBJECTS := $(BUILD_DIR)/obj/engine.o $(BUILD_DIR)/obj/voice.o
LV2_CFLAGS = $(shell $(PKG_CONFIG) --cflags lv2)
# -fno-fast-math comes after OPT: the non-finite guard needs IEEE arithmetic
# even when a host build passes -ffast-math in OPT.
COMPILE_FLAGS = $(CPPFLAGS) $(LV2_CFLAGS) $(CXXFLAGS) $(OPT) -std=c++17 -fPIC \
	-fvisibility=hidden -Wall -Wextra -fno-fast-math -fno-math-errno -fno-trapping-math $(SAN_FLAGS)

.PHONY: all install test test-hosts lint clean
all: $(BUNDLE)/euclidelay.so $(TTL_TARGETS)

$(BUILD_DIR)/obj/%.o: src/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) -MMD -MP -c $< -o $@

$(BUNDLE)/euclidelay.so: $(OBJECTS) src/exports.map
	@mkdir -p "$(@D)"
	$(CXX) $(OBJECTS) $(LDFLAGS) $(SAN_FLAGS) -shared -Wl,--no-undefined \
		-Wl,--version-script=src/exports.map -o $@

$(BUNDLE)/%.ttl: euclidelay.lv2/%.ttl
	@mkdir -p "$(@D)"
	install -m 644 $< $@

$(BUILD_DIR)/test_pattern: tests/test_pattern.cpp tests/patterns_peepo.inc src/pattern.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) $< $(LDFLAGS) $(SAN_FLAGS) -o $@

$(BUILD_DIR)/test_voice: tests/test_voice.cpp $(BUILD_DIR)/obj/voice.o src/voice.hpp src/pattern.hpp src/ports.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) $< $(BUILD_DIR)/obj/voice.o $(LDFLAGS) $(SAN_FLAGS) -o $@

$(BUILD_DIR)/test_voices $(BUILD_DIR)/test_engine $(BUILD_DIR)/test_changes: $(BUILD_DIR)/%: tests/%.cpp tests/engine_run.hpp $(ENGINE_OBJECTS) src/engine.hpp src/voice.hpp src/pattern.hpp src/ports.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) $< $(ENGINE_OBJECTS) $(LDFLAGS) $(SAN_FLAGS) -o $@

$(BUILD_DIR)/test_lv2 $(BUILD_DIR)/test_timing: $(BUILD_DIR)/%: tests/%.cpp src/ports.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) $< $(LDFLAGS) $(SAN_FLAGS) -ldl -o $@

test: all $(TESTS)
	$(PYTHON) tests/ttl_check.py $(BUNDLE)
	$(BUILD_DIR)/test_pattern
	$(BUILD_DIR)/test_voice
	$(BUILD_DIR)/test_voices
	$(BUILD_DIR)/test_engine
	$(BUILD_DIR)/test_changes
	$(BUILD_DIR)/test_lv2 $(BUNDLE)/euclidelay.so
	$(BUILD_DIR)/test_timing $(BUNDLE)/euclidelay.so

# jalv and Ingen on a private dummy JACK server (see tests/host_smoke.py).
test-hosts: all
	$(PYTHON) tests/host_smoke.py $(BUNDLE)

install: all
	install -d "$(DESTDIR)$(LV2DIR)/euclidelay.lv2"
	install -m 755 $(BUNDLE)/euclidelay.so "$(DESTDIR)$(LV2DIR)/euclidelay.lv2/"
	install -m 644 $(TTL_TARGETS) LICENSE "$(DESTDIR)$(LV2DIR)/euclidelay.lv2/"

lint:
	@if command -v lv2lint >/dev/null 2>&1; then \
		LV2_PATH="$(DESTDIR)$(LV2DIR):$${LV2_PATH:-/usr/local/lib/lv2:/usr/lib/lv2}" \
		lv2lint 'https://github.com/ABustos2606/euclidelay#euclidelay'; \
	else echo 'SKIP lv2lint: not installed (install the bundle with make install first)'; fi

clean:
	$(RM) -r $(BUILD_DIR)

-include $(OBJECTS:.o=.d)
