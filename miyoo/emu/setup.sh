#!/bin/bash
# Linux (CI, a cloud session): setup dependencies for Miyoo build and emulator tests.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPS=${DEPS:-$HERE/deps}
mkdir -p "$DEPS"

SUDO=
[ "$(id -u)" != 0 ] && command -v sudo >/dev/null && SUDO=sudo

if ! command -v qemu-arm >/dev/null && ! command -v qemu-arm-static >/dev/null; then
	echo "=== installing qemu-user and test dependencies"
	$SUDO apt-get update -qq
	$SUDO apt-get install -y -qq qemu-user xz-utils unzip curl python3 p7zip-full dnsutils wget >/dev/null
fi

case "$(uname -m)" in
	x86_64) HOST=x86_64 ;;
	aarch64) HOST=aarch64 ;;
	*) echo "no Arm GNU toolchain for $(uname -m)"; exit 1 ;;
esac

TCDIR="$DEPS/arm-gnu-toolchain-13.3.rel1-$HOST-arm-none-linux-gnueabihf"
if [ ! -x "$TCDIR/bin/arm-none-linux-gnueabihf-gcc" ]; then
	echo "=== Arm GNU toolchain 13.3.rel1 ($HOST)"
	if curl -fsSL --retry 3 -o "$DEPS/tc.tar.xz" "https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-$HOST-arm-none-linux-gnueabihf.tar.xz"; then
		tar -xf "$DEPS/tc.tar.xz" -C "$DEPS"
	else
		echo "download failed: using distribution arm-linux-gnueabihf compiler"
		if ! command -v arm-linux-gnueabihf-gcc >/dev/null; then
			$SUDO apt-get install -y -qq gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf >/dev/null
		fi
		TCDIR=$(dirname "$(command -v arm-linux-gnueabihf-gcc)")
	fi
	rm -f "$DEPS/tc.tar.xz"
fi

echo "=== ready: $(command -v qemu-arm || command -v qemu-arm-static), $TCDIR"
