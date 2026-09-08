# G923 macOS driver — build
#
#   make            build everything into build/
#   make test       build + run hardware-free unit tests
#   make e2e        build + run the end-to-end plugin-load test (virtual device)
#   make install    install plugin + daemon under $(PREFIX) and load the agent
#   make uninstall  remove them
#   make clean

PREFIX      ?= $(HOME)/.local/g923
CC          ?= clang
# Universal (x86_64 + arm64): the ForceFeedback plugin must match the host
# process arch, and important hosts are x86_64 — native ETS2 runs under Rosetta,
# and Wine/CrossOver processes are often x86_64. A CFPlugIn only loads if it has
# a slice for the loading process's arch.
ARCHS       ?= -arch arm64 -arch x86_64
CFLAGS      ?= -O2 -Wall -Wextra -Wno-unused-parameter -fno-common $(ARCHS)
COMMON_INC  := -Isrc/common
FRAMEWORKS  := -framework IOKit -framework CoreFoundation
FF_FRAMEWORK:= -framework ForceFeedback

BUILD := build
COMMON_SRC := src/common/g923_effects.c src/common/g923_hid.c \
              src/common/g923_inject.c src/common/g923_find.c \
              src/common/g923_trueforce.c src/common/g923_telemetry_reader.c

SCS_SDK ?=

PLUGIN_BUNDLE := $(BUILD)/G923FF.plugin
PLUGIN_BIN    := $(PLUGIN_BUNDLE)/Contents/MacOS/G923FF

.PHONY: all plugin daemon cli tools tests test e2e clean install uninstall sign scs-plugin scs-plugin-win

all: plugin daemon cli tools tests

$(BUILD):
	@mkdir -p $(BUILD)

# --- ForceFeedback plugin bundle ---
plugin: $(PLUGIN_BIN) sign

$(PLUGIN_BIN): src/plugin/g923_ff_plugin.c $(COMMON_SRC) src/plugin/Info.plist | $(BUILD)
	@mkdir -p $(PLUGIN_BUNDLE)/Contents/MacOS
	@cp src/plugin/Info.plist $(PLUGIN_BUNDLE)/Contents/Info.plist
	$(CC) $(CFLAGS) $(COMMON_INC) -bundle -o $(PLUGIN_BIN) \
		src/plugin/g923_ff_plugin.c $(COMMON_SRC) \
		$(FRAMEWORKS) $(FF_FRAMEWORK) -framework IOKit

sign: $(PLUGIN_BIN)
	@codesign --force --sign - --timestamp=none $(PLUGIN_BUNDLE) 2>/dev/null && \
		echo "signed $(PLUGIN_BUNDLE) (adhoc)" || echo "codesign failed"

# --- daemon ---
daemon: $(BUILD)/g923d
$(BUILD)/g923d: src/daemon/g923d.c $(COMMON_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ src/daemon/g923d.c $(COMMON_SRC) $(FRAMEWORKS)

# --- cli ---
cli: $(BUILD)/g923ctl
$(BUILD)/g923ctl: src/cli/g923ctl.c $(COMMON_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ src/cli/g923ctl.c $(COMMON_SRC) $(FRAMEWORKS)

# --- tools (v2 / diagnostics) ---
tools: $(BUILD)/g923_probe_if2 $(BUILD)/g923_telemetry_dump
$(BUILD)/g923_probe_if2: src/tools/g923_probe_if2.c $(COMMON_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ src/tools/g923_probe_if2.c $(COMMON_SRC) $(FRAMEWORKS)
$(BUILD)/g923_telemetry_dump: src/tools/g923_telemetry_dump.c $(COMMON_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ src/tools/g923_telemetry_dump.c $(COMMON_SRC) $(FRAMEWORKS)

# --- SCS telemetry plugin (needs the official SCS SDK; not built by default) ---
# macOS .dylib for the NATIVE Mac game:
#   make scs-plugin SCS_SDK=/path/to/scs_sdk
scs-plugin: | $(BUILD)
	@test -n "$(SCS_SDK)" || { echo "set SCS_SDK=/path/to/scs_sdk (see src/scs-plugin/README.md)"; exit 1; }
	$(CC) $(CFLAGS) $(COMMON_INC) -I"$(SCS_SDK)/include" -dynamiclib \
		-o $(BUILD)/g923_telemetry.dylib src/scs-plugin/g923_scs_plugin.c
	@echo "built $(BUILD)/g923_telemetry.dylib (universal) — native ETS2 is x86_64/Rosetta, so the x86_64 slice is what it loads. Install per src/scs-plugin/README.md"

# Windows .dll for the game run under CrossOver / Whisky / Wine, cross-compiled
# with mingw-w64 (brew install mingw-w64):
#   make scs-plugin-win SCS_SDK=/path/to/scs_sdk
MINGW ?= x86_64-w64-mingw32-gcc
scs-plugin-win: | $(BUILD)
	@command -v $(MINGW) >/dev/null 2>&1 || { echo "need mingw-w64: brew install mingw-w64 (or set MINGW=<compiler>)"; exit 1; }
	@test -n "$(SCS_SDK)" || { echo "set SCS_SDK=/path/to/scs_sdk (see src/scs-plugin/README.md)"; exit 1; }
	$(MINGW) -O2 -Wall -Wextra -Wno-unused-parameter $(COMMON_INC) -I"$(SCS_SDK)/include" \
		-shared -static-libgcc -o $(BUILD)/g923_telemetry.dll \
		src/scs-plugin/g923_scs_plugin.c -Wl,--enable-stdcall-fixup
	@echo "built $(BUILD)/g923_telemetry.dll — install into the bottle per src/scs-plugin/README.md"

# --- tests ---
tests: $(BUILD)/test_protocol $(BUILD)/test_telemetry $(BUILD)/ff_probe
$(BUILD)/test_protocol: src/test/test_protocol.c $(COMMON_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ src/test/test_protocol.c $(COMMON_SRC) $(FRAMEWORKS)
$(BUILD)/test_telemetry: src/test/test_telemetry.c $(COMMON_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ src/test/test_telemetry.c $(COMMON_SRC) $(FRAMEWORKS)
$(BUILD)/ff_probe: src/test/ff_probe.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/test/ff_probe.c $(FRAMEWORKS) $(FF_FRAMEWORK)

test: tests
	@echo; ./$(BUILD)/test_protocol
	@echo; ./$(BUILD)/test_telemetry

e2e: all
	@bash scripts/test_e2e.sh

install: all
	@bash scripts/install.sh "$(PREFIX)"

uninstall:
	@bash scripts/uninstall.sh "$(PREFIX)"

clean:
	rm -rf $(BUILD)
