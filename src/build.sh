#!/bin/sh
# builds the port manager for the Miyoo (static, Cortex-A7): src/build.sh [toolchain bin folder]
# default toolchain: the Arm GNU toolchain 13.3 in redriver2-miyoo's deps/
cd "$(dirname "$0")" || exit 1
TC=${1:-/g/Emulacja/redriver2-miyoo/deps/arm-gnu-toolchain-13.3.rel1-mingw-w64-i686-arm-none-linux-gnueabihf/bin}
OUT=../onion/App/PortManager/portmaster
"$TC/arm-none-linux-gnueabihf-gcc" -O2 -Wall -Wextra -D_GNU_SOURCE -static -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
	-o "$OUT" main.c gfx.c util.c pm.c vendor/cJSON.c vendor/miniz.c vendor/sha256.c &&
"$TC/arm-none-linux-gnueabihf-strip" "$OUT" && ls -l "$OUT"
