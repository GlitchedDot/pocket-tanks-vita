#!/bin/bash
# data_prep.sh — prepare Pocket Tanks game data for the PS Vita port.
#
# Usage: ./data_prep.sh /path/to/pocket-tanks-3.0.0.apk
#
# Produces ./pockettanks-data/ with:
#   libengine.so libfmod.so libc++_shared.so libsqlcipher.so
#   assets/  (music/*.m4a converted to .ogg)
#   res/
#   cache/   (empty, created on device if missing)
#
# Then copy the CONTENTS of pockettanks-data/ to ux0:data/pockettanks/
# on your Vita via VitaShell USB or FTP.
#
# Requires: unzip, ffmpeg (for the M4A -> OGG music conversion).
set -e

if [ $# -lt 1 ]; then
    echo "Usage: $0 /path/to/pocket-tanks.apk"
    exit 1
fi

APK="$1"
OUT="pockettanks-data"

if [ ! -f "$APK" ]; then
    echo "APK not found: $APK"
    exit 1
fi

command -v unzip >/dev/null || { echo "need unzip"; exit 1; }
command -v ffmpeg >/dev/null || { echo "need ffmpeg (for music conversion)"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/assets" "$OUT/res" "$OUT/cache"

echo "== extracting native libs =="
unzip -p "$APK" lib/armeabi-v7a/libengine.so    > "$OUT/libengine.so"
unzip -p "$APK" lib/armeabi-v7a/libfmod.so      > "$OUT/libfmod.so"
unzip -p "$APK" lib/armeabi-v7a/libc++_shared.so > "$OUT/libc++_shared.so"
# present in APK; ship in case something references it
if unzip -l "$APK" | grep -q "lib/armeabi-v7a/libsqlcipher.so"; then
    unzip -p "$APK" lib/armeabi-v7a/libsqlcipher.so > "$OUT/libsqlcipher.so"
fi

echo "== extracting assets =="
unzip -q -o "$APK" 'assets/*' -d "$OUT.tmp"
mv "$OUT.tmp/assets"/* "$OUT/assets/"
rm -rf "$OUT.tmp"

echo "== extracting res =="
if unzip -l "$APK" | grep -q '^.*res/'; then
    unzip -q -o "$APK" 'res/*' -d "$OUT.tmp"
    mv "$OUT.tmp/res"/* "$OUT/res/" 2>/dev/null || true
    rm -rf "$OUT.tmp"
fi

echo "== converting music M4A -> OGG =="
for m in "$OUT"/assets/music/*.m4a; do
    [ -e "$m" ] || continue
    base="${m%.m4a}"
    echo "  $(basename "$m") -> $(basename "$base").ogg"
    ffmpeg -y -loglevel error -i "$m" -c:a libvorbis -q:a 4 "$base.ogg"
    rm "$m"
done

echo ""
echo "Done. Copy the CONTENTS of ./$OUT/ to ux0:data/pockettanks/ on your Vita."
echo "Then install pockettanks.vpk with VitaShell."
echo ""
echo "Required on the Vita: kubridge.skprx and fd_fix.skprx (kernel plugins)."
