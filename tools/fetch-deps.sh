#!/bin/bash
# Fetches macOS build dependencies into .deps/ (ignored by git):
#   .deps/obs-studio : libobs + frontend API headers of the OBS version we build against
#   .deps/qt6        : the prebuilt universal Qt package OBS itself ships with (hash-verified)
set -euo pipefail

OBS_VERSION="${OBS_VERSION:-32.2.2}"
DEPS_VERSION="${DEPS_VERSION:-2026-07-15}"
QT_HASH="${QT_HASH:-d4b8058612a7067e44b2205fe7925ee24e9ec6b15ac8c29d3e6230c70030b102}"

root="$(cd "$(dirname "$0")/.." && pwd)"
deps="$root/.deps"
mkdir -p "$deps"

if [ ! -f "$deps/obs-studio/libobs/obs.h" ]; then
  echo "Fetching OBS $OBS_VERSION headers..."
  git clone --depth 1 --branch "$OBS_VERSION" --filter=blob:none --sparse \
    https://github.com/obsproject/obs-studio.git "$deps/obs-studio"
  git -C "$deps/obs-studio" sparse-checkout set libobs deps/w32-pthreads frontend/api
fi

# SIMDe (header-only; libobs uses it on Apple silicon) - same commit as OBS's own dependency recipe
SIMDE_COMMIT="${SIMDE_COMMIT:-71fd833d9666141edcd1d3c109a80e228303d8d7}"
if [ ! -f "$deps/simde/simde/x86/sse2.h" ]; then
  echo "Fetching SIMDe..."
  rm -rf "$deps/simde"
  git init -q "$deps/simde"
  git -C "$deps/simde" fetch -q --depth 1 https://github.com/simd-everywhere/simde.git "$SIMDE_COMMIT"
  git -C "$deps/simde" checkout -q FETCH_HEAD
fi

if [ ! -d "$deps/qt6/lib/cmake/Qt6" ]; then
  archive="$deps/qt6.tar.xz"
  url="https://github.com/obsproject/obs-deps/releases/download/$DEPS_VERSION/macos-deps-qt6-$DEPS_VERSION-universal.tar.xz"
  echo "Downloading Qt package $DEPS_VERSION..."
  curl -fsSL "$url" -o "$archive"
  echo "$QT_HASH  $archive" | shasum -a 256 -c -
  mkdir -p "$deps/qt6"
  tar -xJf "$archive" -C "$deps/qt6"
  rm "$archive"
fi
echo "Dependencies ready in $deps"
