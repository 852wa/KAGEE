#!/bin/bash
# Builds Kagee for macOS (universal: Apple silicon + Intel) and creates in dist/:
#   Kagee-<ver>-macos-universal.pkg   installer -> ~/Library/Application Support/obs-studio/plugins
#   Kagee-<ver>-macos-universal.zip   kagee.plugin for manual installation
#   SHA256SUMS-macos.txt
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
ver="$(sed -n 's/^project(kagee VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
echo "Kagee $ver (macOS)"

"$root/tools/fetch-deps.sh"
cmake -S "$root" -B "$root/build_macos" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build "$root/build_macos" --config Release -j "$(sysctl -n hw.ncpu)"

dist="$root/dist"
rm -rf "$dist"
mkdir -p "$dist/stage"
cp -R "$root/build_macos/kagee.plugin" "$dist/stage/"
# ad-hoc signature (required for arm64 code; no developer identity)
codesign --force --deep --sign - "$dist/stage/kagee.plugin"
lipo -archs "$dist/stage/kagee.plugin/Contents/MacOS/kagee"

name="Kagee-$ver-macos-universal"
ditto -c -k --keepParent "$dist/stage/kagee.plugin" "$dist/$name.zip"

# component package; installed relative to the user's home (see distribution.xml)
pkgbuild --analyze --root "$dist/stage" "$dist/component.plist"
plutil -replace 0.BundleIsRelocatable -bool NO "$dist/component.plist"
pkgbuild --root "$dist/stage" --component-plist "$dist/component.plist" \
  --identifier com.hakoniwa.kagee --version "$ver" \
  --install-location "/Library/Application Support/obs-studio/plugins" \
  "$dist/kagee-component.pkg"
sed "s/@VERSION@/$ver/g" "$root/installer/macos/distribution.xml" > "$dist/distribution.xml"
mkdir -p "$dist/resources"
cp "$root/installer/macos/resources/"* "$dist/resources/"
cp "$root/LICENSE" "$dist/resources/LICENSE.txt"
productbuild --distribution "$dist/distribution.xml" \
  --resources "$dist/resources" \
  --package-path "$dist" "$dist/$name.pkg"
rm -rf "$dist/component.plist" "$dist/kagee-component.pkg" "$dist/distribution.xml" "$dist/resources"

( cd "$dist" && shasum -a 256 "$name.pkg" "$name.zip" > SHA256SUMS-macos.txt )
ls -l "$dist"
