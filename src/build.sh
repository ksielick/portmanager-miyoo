#!/bin/sh
# builds the port manager for the Miyoo (static, Cortex-A7): src/build.sh [toolchain bin folder]
# With EMU=1: builds the emulator binary with emu_shim.c
cd "$(dirname "$0")" || exit 1

if [ -n "$1" ]; then
	TC=$1
elif [ -z "$TC" ]; then
	for cand in \
		/g/Emulacja/redriver2-miyoo/deps/arm-gnu-toolchain-13.3.rel1-mingw-w64-i686-arm-none-linux-gnueabihf/bin \
		../miyoo/emu/deps/arm-gnu-toolchain-13.3.rel1-*/bin \
		/usr/bin; do
		if [ -x "$cand/arm-none-linux-gnueabihf-gcc" ] || [ -x "$cand/arm-none-linux-gnueabihf-gcc.exe" ] || [ -x "$cand/arm-linux-gnueabihf-gcc" ]; then
			TC=$cand
			break
		fi
	done
fi

if [ -x "$TC/arm-none-linux-gnueabihf-gcc" ] || [ -x "$TC/arm-none-linux-gnueabihf-gcc.exe" ]; then
	CC="$TC/arm-none-linux-gnueabihf-gcc"
	STRIP="$TC/arm-none-linux-gnueabihf-strip"
elif [ -x "$TC/arm-linux-gnueabihf-gcc" ]; then
	CC="$TC/arm-linux-gnueabihf-gcc"
	STRIP="$TC/arm-linux-gnueabihf-strip"
elif command -v arm-none-linux-gnueabihf-gcc >/dev/null 2>&1; then
	CC=arm-none-linux-gnueabihf-gcc
	STRIP=arm-none-linux-gnueabihf-strip
elif command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
	CC=arm-linux-gnueabihf-gcc
	STRIP=arm-linux-gnueabihf-strip
else
	echo "no ARM compiler found in TC=$TC or PATH" >&2
	exit 1
fi

SRCS="main.c gfx.c util.c pm.c vendor/cJSON.c vendor/miniz.c vendor/sha256.c"
CFLAGS="-O2 -Wall -Wextra -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -static -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"

if [ "$EMU" = "1" ]; then
	OUT=${OUT:-../build/miyoo_emu/portmanager_emu}
	mkdir -p "$(dirname "$OUT")"
	"$CC" $CFLAGS -Wl,--wrap=open,--wrap=open64,--wrap=ioctl,--wrap=read \
		-o "$OUT" $SRCS ../miyoo/emu/emu_shim.c -lm -lpthread &&
	cp -f ../catalog.json "$(dirname "$OUT")/catalog.json" &&
	[ -f ../onion/App/PortManager/cacert.pem ] && cp -f ../onion/App/PortManager/cacert.pem "$(dirname "$OUT")/cacert.pem"
	ls -l "$OUT"
else
	OUT=${OUT:-../onion/App/PortManager/portmanager}
	mkdir -p "$(dirname "$OUT")"
	"$CC" $CFLAGS -o "$OUT" $SRCS &&
	"$STRIP" "$OUT" &&
	cp -f ../catalog.json "$(dirname "$OUT")/catalog.json" &&
	{ [ ! -f ../catalog.local.json ] || cp -f ../catalog.local.json "$(dirname "$OUT")/catalog.local.json"; } &&
	ls -l "$OUT"
fi
