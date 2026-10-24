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

Licence: MIT (see `LICENSE`).
