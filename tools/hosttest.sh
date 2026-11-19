#!/usr/bin/env bash
# Host-side unit tests for the device-independent core.
#
#   tools/hosttest.sh          build with gcc and run
#   tools/hosttest.sh asan     build with clang -fsanitize=address,undefined
#
# MSYS2's gcc is built without libsanitizer, so the sanitizer pass uses clang.
set -e

cd "$(dirname "$0")/.."
mkdir -p build

SRC="src/term.c src/vt.c src/config.c src/fileio.c src/vault.c test/vault_platform_stub.c test/harness.c"

if [ "$1" = "asan" ]; then
	export PATH="/c/msys64/clang64/bin:$PATH"
	out=build/hosttest_asan.exe
	echo "clang $(clang --version | head -1)"
	clang -std=c11 -g -O1 -Wall -Wextra -Isrc -fsanitize=address,undefined \
		$SRC -o "$out"
else
	out=build/hosttest.exe
	echo "gcc $(gcc --version | head -1)"
	gcc -std=c11 -g -O1 -Wall -Wextra -Isrc $SRC -o "$out"
fi

"$out"
