# SnapI2CHID

The I2C-HID keyboard, touchpad and precision-touchpad driver used by
[Snapintosh](https://github.com/Snapintosh/Snapintosh) on the ASUS Zenbook A16 (Snapdragon X2 Elite). It drives the
GENI I2C controller and publishes HID devices to macOS, including multitouch for trackpad gestures.

This repository is mounted in the Snapintosh umbrella at `drivers/snap-i2c-hid`. Build it with `./build.sh`
(Xcode with the macOS 27 SDK). The kext is linked into Snapintosh's auxiliary kernel collection by the umbrella's
tools; see the umbrella's `docs/BUILDING.md` and `AGENTS.md`.

`PROFILE=<name> ./build.sh` builds a named configuration from `profiles/<name>.sh` into `out/<name>/` (or `OUT`),
with each unit's own define set, and records the sets in `DEFINES.txt`. Without `PROFILE` the build is unchanged.
`desktop` is the deployed desktop SnapI2CHID d90e0199: `-DSNAP_I2C_HID_LATENCY=1` in `SnapI2CHID.o` only. It matches
that kext section for section (the umbrella's `tools/macho-sections.py`); the file bytes also carry object paths,
object times and a UUID.

`desktop-fnkeys` is `desktop` plus the keyboard's Fn-key layer, `-DSNAP_I2C_HID_FNKEYS=1` in `SnapI2CHID.o` and
`hid-keyboard.o` (contract and evidence in `src/hid-keyboard.h`). Built in, it stays off until the guest boot-arg
`snap_i2c_fnkeys` sets bits: `0x1` turns the keyboard's ASUS hotkey report (input report 90, vendor page 0xFF31) and
its Consumer brightness usages into what macOS's keyboards send (display brightness as Apple top-case
0x00FF/0x0005 down and 0x0004 up); `0x2` logs, rate-limited, what the F-row and unknown reports send (hotkey codes,
Consumer usages, F-row and navigation keys, never a key that types a character); `0x4` (with `0x1`) swaps F1-F12 and
their hotkeys in the host on Fn+Esc; `0x8`/`0x10` pick another brightness encoding (Apple vendor keyboard
0xFF01/0x0021-0x0020, or Consumer 0x0070-0x006F). A refused set (unknown bits, encoding 3, `0x4` without `0x1`)
leaves the layer off. Without the define the default and `desktop` builds are section for section the earlier ones.

Licence: MIT (see `LICENSE`).
