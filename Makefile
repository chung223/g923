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
CFLAGS      ?= -O2 -Wall -Wextra -Wno-unused-parameter -fno-common
COMMON_INC  := -Isrc/common
FRAMEWORKS  := -framework IOKit -framework CoreFoundation
FF_FRAMEWORK:= -framework ForceFeedback

BUILD := build
COMMON_SRC := src/common/g923_effects.c src/common/g923_hid.c \
              src/common/g923_inject.c src/common/g923_find.c

PLUGIN_BUNDLE := $(BUILD)/G923FF.plugin
PLUGIN_BIN    := $(PLUGIN_BUNDLE)/Contents/MacOS/G923FF

.PHONY: all plugin daemon cli tests test e2e clean install uninstall sign

all: plugin daemon cli tests

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

# --- tests ---
tests: $(BUILD)/test_protocol $(BUILD)/ff_probe
$(BUILD)/test_protocol: src/test/test_protocol.c src/common/g923_effects.c | $(BUILD)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ src/test/test_protocol.c src/common/g923_effects.c
$(BUILD)/ff_probe: src/test/ff_probe.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/test/ff_probe.c $(FRAMEWORKS) $(FF_FRAMEWORK)

test: tests
	@echo; ./$(BUILD)/test_protocol

e2e: all
	@bash scripts/test_e2e.sh

install: all
	@bash scripts/install.sh "$(PREFIX)"

uninstall:
	@bash scripts/uninstall.sh "$(PREFIX)"

clean:
	rm -rf $(BUILD)
