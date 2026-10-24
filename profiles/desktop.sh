# SPDX-License-Identifier: MIT
# Sourced by build.sh (PROFILE=desktop). The A16 desktop's SnapI2CHID: input-latency polling (-DSNAP_I2C_HID_LATENCY=1)
# in SnapI2CHID.o only; geni-i2c, the HID parsers and i2c-hid take no define. The configuration of SnapI2CHID d90e0199,
# linked into every desktop AuxKC since b12v12. Section for section equal to that kext (the umbrella's
# tools/macho-sections.py; the file bytes also carry object paths, object times and a UUID).
U_SnapI2CHID="-DSNAP_I2C_HID_LATENCY=1"
