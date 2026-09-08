#!/bin/bash
# Remove the G923 driver for the current user.
#   scripts/uninstall.sh [PREFIX]   (default: ~/.local/g923)
set -uo pipefail
PREFIX="${1:-$HOME/.local/g923}"
AGENT_LABEL="dev.g923.g923d"
AGENT_PLIST="$HOME/Library/LaunchAgents/$AGENT_LABEL.plist"

echo "Stopping agent"
launchctl unload "$AGENT_PLIST" 2>/dev/null || true
rm -f "$AGENT_PLIST"

# Best-effort: clear the injected FF plugin entry from any attached wheel.
if [ -x "$PREFIX/g923ctl" ]; then "$PREFIX/g923ctl" clear 2>/dev/null || true; fi

echo "Removing $PREFIX"
rm -rf "$PREFIX/G923FF.plugin" "$PREFIX/g923d" "$PREFIX/g923ctl" "$PREFIX/g923d.log"
rmdir "$PREFIX" 2>/dev/null || true
echo "Done. (Unplug/replug the wheel to drop the in-registry FF registration if it lingers.)"
