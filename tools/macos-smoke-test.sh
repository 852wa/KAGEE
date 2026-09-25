#!/bin/bash
# Loads dist/stage/kagee.plugin into the libobs of a real OBS Studio app bundle and checks it.
#   OBS_APP=/path/to/OBS.app tools/macos-smoke-test.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
app="${OBS_APP:?set OBS_APP to an OBS.app}"
fw="$app/Contents/Frameworks"
frontend="$(find "$fw" -maxdepth 2 -name 'obs-frontend-api*.dylib' | head -n 1)"
echo "libobs:   $fw/libobs.framework"
echo "frontend: $frontend"

clang -O1 -o "$root/build_macos/kagee-smoke" "$root/tools/macos-smoke-test.c" \
  -I "$root/.deps/obs-studio/libobs" -I "$root/.deps/simde" -I "$root/sdk/include" -DHAVE_OBSCONFIG_H \
  -F "$fw" -framework libobs "$frontend" \
  -Wl,-rpath,"$fw" -mmacosx-version-min=13.0

"$root/build_macos/kagee-smoke" "$root/dist/stage/kagee.plugin"
