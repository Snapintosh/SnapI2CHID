# SnapI2CHID

The I2C-HID keyboard, touchpad and precision-touchpad driver used by
[Snapintosh](https://github.com/Snapintosh/Snapintosh) on the ASUS Zenbook A16 (Snapdragon X2 Elite). It drives the
GENI I2C controller and publishes HID devices to macOS, including multitouch for trackpad gestures.

This repository is mounted in the Snapintosh umbrella at `drivers/snap-i2c-hid`. Build it with `./build.sh`
(Xcode with the macOS 27 SDK). The kext is linked into Snapintosh's auxiliary kernel collection by the umbrella's
tools; see the umbrella's `docs/BUILDING.md` and `AGENTS.md`.

Licence: MIT (see `LICENSE`).
