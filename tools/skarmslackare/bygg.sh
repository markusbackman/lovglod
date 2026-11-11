#!/bin/zsh
# Bygger LovGlod.saver (visas som LövGlöd) av produktfilmen. Kör: tools/skarmslackare/bygg.sh [--installera]
set -euo pipefail

har="${0:A:h}"
film="${FILM:-$har/../produktfilm/ut/lovglod-produktfilm-final.mp4}"
ut="$har/ut"
# Filnamnet måste vara ASCII: WallpaperAgent hittar inte en modul med å/ä/ö i sökvägen.
saver="$ut/LovGlod.saver"

rm -rf "$ut"
mkdir -p "$saver/Contents/MacOS" "$saver/Contents/Resources" "$ut/obj"

for arch in arm64 x86_64; do
  swiftc -O -parse-as-library -module-name LovGlod \
    -target "$arch-apple-macos13" \
    -Xlinker -bundle \
    -o "$ut/obj/LovGlod-$arch" "$har/LovGlodView.swift"
done
lipo -create -output "$saver/Contents/MacOS/LovGlod" "$ut"/obj/LovGlod-*
rm -rf "$ut/obj"

cp "$film" "$saver/Contents/Resources/produktfilm.mp4"

# Miniatyren i Systeminställningar (90×58 och 180×116), en bildruta ur filmen.
bild="${BILD_TID:-20}"
for skala in "thumbnail:90:58" "thumbnail@2x:180:116"; do
  namn="${skala%%:*}"; rest="${skala#*:}"; b="${rest%%:*}"; h="${rest#*:}"
  ffmpeg -v error -y -ss "$bild" -i "$film" -frames:v 1 \
    -vf "scale=$b:$h:force_original_aspect_ratio=increase,crop=$b:$h" \
    "$saver/Contents/Resources/$namn.png"
done

cat > "$saver/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>lovglod.screensaver</string>
  <key>CFBundleName</key><string>LövGlöd</string>
  <key>CFBundleDisplayName</key><string>LövGlöd</string>
  <key>CFBundleExecutable</key><string>LovGlod</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>NSPrincipalClass</key><string>LovGlodView</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
</dict>
</plist>
PLIST

codesign --force --sign - "$saver"
echo "Byggd: $saver"

if [[ "${1:-}" == "--installera" ]]; then
  mal="$HOME/Library/Screen Savers"
  mkdir -p "$mal"
  rm -rf "$mal/LovGlod.saver" "$mal/LövGlöd.saver"
  cp -R "$saver" "$mal/"
  killall legacyScreenSaver 2>/dev/null || true
  echo "Installerad: $mal/LovGlod.saver"
fi
