#!/bin/bash
# Install the G923 driver for the current user (no root, SIP stays on).
#   scripts/install.sh [PREFIX]   (default: ~/.local/g923)
set -euo pipefail
cd "$(dirname "$0")/.."
PREFIX="${1:-$HOME/.local/g923}"
BUILD="build"
AGENT_LABEL="dev.g923.g923d"
AGENT_PLIST="$HOME/Library/LaunchAgents/$AGENT_LABEL.plist"

[ -d "$BUILD/G923FF.plugin" ] || { echo "run 'make' first"; exit 1; }

echo "Installing to $PREFIX"
mkdir -p "$PREFIX"
rm -rf "$PREFIX/G923FF.plugin"
cp -R "$BUILD/G923FF.plugin" "$PREFIX/"
cp "$BUILD/g923d" "$PREFIX/"
cp "$BUILD/g923ctl" "$PREFIX/"

# Re-sign the plugin adhoc at its final location (signature covers the path-independent bundle).
codesign --force --sign - "$PREFIX/G923FF.plugin" >/dev/null 2>&1 || true

PLUGIN_ABS="$PREFIX/G923FF.plugin"
echo "Writing LaunchAgent -> $AGENT_PLIST"
mkdir -p "$HOME/Library/LaunchAgents"
cat > "$AGENT_PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$AGENT_LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$PREFIX/g923d</string>
    <string>--plugin</string>
    <string>$PLUGIN_ABS</string>
    <string>--range</string>
    <string>900</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$PREFIX/g923d.log</string>
  <key>StandardErrorPath</key><string>$PREFIX/g923d.log</string>
</dict>
</plist>
PLIST

echo "Loading agent"
launchctl unload "$AGENT_PLIST" 2>/dev/null || true
launchctl load "$AGENT_PLIST"

cat <<EOF

Installed.
  binaries : $PREFIX/{g923d,g923ctl}
  plugin   : $PLUGIN_ABS
  agent    : $AGENT_PLIST (log: $PREFIX/g923d.log)

Next:
  1. Plug in the wheel.
  2. Run:  $PREFIX/g923ctl list
     - if it shows "PlayStation mode", run: $PREFIX/g923ctl mode-native  (or let g923d do it)
  3. Test force: $PREFIX/g923ctl force 20000   (wheel should tug ~2s)

Note: hardened games that enforce library validation will not load an adhoc
plugin. CrossOver / Whisky / Wine / Game Porting Toolkit DO load it. For broad
native-app support, sign G923FF.plugin with a Developer ID and notarize it.
EOF
