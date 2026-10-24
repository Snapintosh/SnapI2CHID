# SPDX-License-Identifier: MIT
# Sourced by build.sh (PROFILE=desktop-fnkeys). The desktop profile plus the keyboard's Fn-key layer
# (-DSNAP_I2C_HID_FNKEYS=1) in SnapI2CHID.o and hid-keyboard.o, which both carry it (hid-keyboard.h). Built in, the
# layer still does nothing until the guest boot-arg snap_i2c_fnkeys sets HID_FNKEYS_* bits (tools/cycle.py
# --input-fnkeys). Not deployed yet: no A16 run has used it.
U_SnapI2CHID="-DSNAP_I2C_HID_LATENCY=1 -DSNAP_I2C_HID_FNKEYS=1"
U_hid_keyboard="-DSNAP_I2C_HID_FNKEYS=1"
