#!/bin/sh
# SPDX-License-Identifier: MIT
# Build SnapI2CHID.kext (arm64e, MH_KEXT_BUNDLE) against the SDK Kernel.framework,
# the same toolchain as gpu/adreno-kext/kext/build.sh. kmutil resolves the
# undefined IOHIDFamily/kernel symbols when the AuxKC is linked
# (tools/build-auxkc-user.py). No KDK is needed for this step. include/ holds an
# empty IOKit/hid/IOHIDPrivateKeys.h: the SDK's IOHIDDevice.h includes it but the
# SDK does not ship it, and nothing this driver uses comes from it.
#
# PROFILE=<name> sh build.sh builds a named configuration from profiles/<name>.sh: each translation unit gets the
# profile's own define set and the link is unchanged. Without PROFILE the build is exactly the default one.
# Output: OUT, else out (default) or out/<name>.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
PROFILE=${PROFILE:-}
O=${OUT:-$D/out${PROFILE:+/$PROFILE}}; OBJ=$O/obj
SDK=$(xcrun --show-sdk-path)
KH=$SDK/System/Library/Frameworks/Kernel.framework/Headers
mkdir -p "$OBJ"
# Per-unit defines, empty in the default build.
U_SnapI2CHID= U_geni_i2c= U_hid_rdesc= U_i2c_hid= U_hid_keyboard= U_hid_pointer= U_hid_ptp= U_hid_mt2= U_kmod_info=
if [ -n "$PROFILE" ]; then
    case "$PROFILE" in */*|.*) echo "error: PROFILE is a name in $D/profiles" >&2; exit 2;; esac
    [ -f "$D/profiles/$PROFILE.sh" ] || { echo "error: no profile $D/profiles/$PROFILE.sh" >&2; exit 2; }
    . "$D/profiles/$PROFILE.sh"
fi
COMMON="-target arm64e-apple-macos27.0 -isysroot $SDK -mkernel -nostdinc -I$KH -I$D/src -I$D/include -DKERNEL -DAPPLE -DNeXT -O2 -g -Wall -Werror -fno-common"
CXXF="$COMMON -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -Wno-inconsistent-missing-override"
xcrun clang++ $CXXF $U_SnapI2CHID -c "$D/src/SnapI2CHID.cpp" -o "$OBJ/SnapI2CHID.o"
for f in geni-i2c hid-rdesc i2c-hid hid-keyboard hid-pointer hid-ptp hid-mt2; do
  eval "defs=\$U_$(echo "$f" | tr - _)"
  xcrun clang $COMMON -std=gnu11 $defs -c "$D/src/$f.c" -o "$OBJ/$f.o"
done
xcrun clang $COMMON $U_kmod_info -c "$D/src/kmod_info.c" -o "$OBJ/kmod_info.o"
K=$O/SnapI2CHID.kext
rm -rf "$K"; mkdir -p "$K/Contents/MacOS"
cp "$D/Info.plist" "$K/Contents/Info.plist"
xcrun clang++ -target arm64e-apple-macos27.0 -isysroot "$SDK" -mkernel -nostdlib -Xlinker -kext \
  -Xlinker -export_dynamic -o "$K/Contents/MacOS/SnapI2CHID" "$OBJ/SnapI2CHID.o" \
  "$OBJ/geni-i2c.o" "$OBJ/hid-rdesc.o" "$OBJ/i2c-hid.o" "$OBJ/hid-keyboard.o" "$OBJ/hid-pointer.o" "$OBJ/hid-ptp.o" "$OBJ/hid-mt2.o" \
  "$OBJ/kmod_info.o" \
  -L"$SDK/usr/lib" -lkmodc++ -lkmod -lcc_kext
codesign -f -s - "$K"
if [ -n "$PROFILE" ]; then
    # The define sets this build used, unit by unit.
    {
        echo "profile $PROFILE"
        for u in SnapI2CHID geni_i2c hid_rdesc i2c_hid hid_keyboard hid_pointer hid_ptp hid_mt2 kmod_info; do
            eval "set -- \$U_$u"; echo "$u: $*"
        done
    } > "$O/DEFINES.txt"
fi
echo "kext: $K"
