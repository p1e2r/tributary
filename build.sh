#!/usr/bin/env bash
# Build Tributary on macOS or Linux.
#   macOS:  brew install cmake ninja molten-vk vulkan-headers
#   Linux:  sudo apt install cmake ninja-build g++ libx11-dev libssl-dev libvulkan-dev
set -euo pipefail
cd "$(dirname "$0")"
ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release)
if [[ "$(uname)" == "Darwin" ]]; then
  ARGS+=(-DTRIBUTARY_MOLTENVK="$(brew --prefix molten-vk)/lib/libMoltenVK.dylib")
  ARGS+=(-DVULKAN_HEADERS_DIR="$(brew --prefix vulkan-headers)/include")
fi
cmake -B build "${ARGS[@]}"
cmake --build build
if [[ "$(uname)" == "Darwin" ]]; then
  codesign --force --deep --sign - build/Tributary.app >/dev/null 2>&1 || true
  echo "Built build/Tributary.app — run: open build/Tributary.app"
else
  echo "Built build/Tributary — run: ./build/Tributary"
fi
