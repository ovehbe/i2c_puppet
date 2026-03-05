#!/bin/bash
# Build i2c_puppet with BACKLIGHT_IGNORE_HOST + BACKLIGHT_PERSIST (Sym+0 toggle, save state).
# Requires: cmake, gcc-arm-none-eabi, and 3rdparty/pico-sdk (clone if missing).

set -e
cd "$(dirname "$0")"

# Ensure pico-sdk is present
if [ ! -f 3rdparty/pico-sdk/pico_sdk_init.cmake ]; then
  echo "Pico SDK not found. Cloning..."
  mkdir -p 3rdparty
  (cd 3rdparty && rm -rf pico-sdk && git clone --depth 1 https://github.com/raspberrypi/pico-sdk.git && cd pico-sdk && git submodule update --init)
fi

mkdir -p build
cd build
cmake -DPICO_BOARD=bbq20kbd_breakout \
      -DCMAKE_BUILD_TYPE=Release \
      -DBACKLIGHT_IGNORE_HOST=ON \
      -DBACKLIGHT_PERSIST=ON \
      ..
make -j$(nproc)

UF2="$(pwd)/i2c_puppet.uf2"
[ -f "app/i2c_puppet.uf2" ] && UF2="$(pwd)/app/i2c_puppet.uf2"
echo ""
echo "Done. UF2 for flashing:"
echo "  $UF2"
ls -la "$UF2" 2>/dev/null || true
