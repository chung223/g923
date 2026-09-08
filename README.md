# g923-mac — Logitech G923 force feedback for macOS (Apple Silicon, SIP on)

A user-space driver that gives the Logitech **G923** (and G29-family) wheel
**force feedback** on modern macOS **without a kext and without disabling SIP**.

- Inputs (steering, pedals, buttons) already work through macOS's generic HID.
- This project adds the missing pieces: **force feedback**, **rotation range**,
  **LEDs**, **auto-center**, and the **PlayStation→native mode switch**.
- It does **not** provide TrueForce (that channel is proprietary to Logitech's
  Windows SDK). You get classic FFB: constant force, spring, damper, friction,
  and periodic waves — enough for real sim-racing road feel and wheel centering.

See [`docs/research-report.md`](docs/research-report.md) for the full research,
the exact protocol bytes, and how the macOS load path works.

## How it works (short version)

Games talk to Apple's `ForceFeedback.framework`, which loads a per-device
plug-in. We supply that plug-in (`G923FF.plugin`) and translate DirectInput-style
effects into Logitech's classic HID commands, sent straight to the wheel.

The framework normally only looks for plug-ins under the sealed, read-only
`/System/Library/Extensions/`. A small agent (`g923d`) registers our plug-in on
the wheel's IOKit node using a path that escapes that directory with `../`, so
the plug-in can live in your home folder. This was verified working on
macOS 26.2 / Apple Silicon with SIP enabled, as a normal user.

## Who gets force feedback

| Host running the game | FFB works? |
|---|---|
| **CrossOver / Whisky / Wine / Game Porting Toolkit** (Windows sim-racing) | **Yes** |
| Native apps/SDL games built without library validation | Yes |
| Native games with hardened runtime **+ library validation** | No (needs a Developer-ID-signed, notarized plug-in — and the game may still refuse) |

## Build & test (no wheel needed)

```bash
make            # build plugin, daemon, CLI
make test       # 42 hardware-free unit tests (protocol + effects math)
make e2e        # end-to-end: real ForceFeedback.framework loads the real plugin
```

## Install (per-user, SIP stays on)

```bash
make install    # installs to ~/.local/g923 and loads a LaunchAgent
```

Then, with the wheel plugged in:

```bash
~/.local/g923/g923ctl list          # shows the wheel + whether FFB is wired up
~/.local/g923/g923ctl mode-native   # only if it reports "PlayStation mode"
~/.local/g923/g923ctl force 20000   # wheel should pull for ~2s
~/.local/g923/g923ctl range 900     # set rotation range in degrees
```

Uninstall: `make uninstall`.

## Layout

```
src/common/    protocol encoders, effect engine, HID transport, injection, discovery
src/plugin/    G923FF.plugin — the ForceFeedback CFPlugIn
src/daemon/    g923d — LaunchAgent that registers the plugin on wheel hotplug
src/cli/       g923ctl — control & diagnostics
src/test/      unit tests + ff_probe (a stand-in "game")
scripts/       install / uninstall / e2e test
docs/          research report (Traditional Chinese)
```

## Status

Everything except the on-wheel behavior is built and verified on this Mac.
What still needs the physical wheel: confirming shared-open output reports,
any Input-Monitoring TCC prompt, mode-switch re-enumeration timing, and FFB
feel. See the report's "風險與未知" section.

## Credit / sources

Logitech protocol from the GPL-2.0 `berarma/new-lg4ff` Linux driver and the
Linux mainline HID drivers; macOS internals from Apple's open-source
`IOKitUser` / `IOHIDFamily` and the `ForceFeedback` framework headers.
