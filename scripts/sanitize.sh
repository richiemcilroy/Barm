#!/bin/sh
# Runs a Barm program (or its tests) under AddressSanitizer + LeakSanitizer + UBSan in a Linux
# container (ASan hangs at startup on recent macOS). Usage:
#   scripts/sanitize.sh run  <path>     # build and run main()
#   scripts/sanitize.sh test <path>     # build and run tests
set -eu
mode=$1
path=$2
root=$(cd "$(dirname "$0")/.." && pwd)
barm="$root/target/release/barm"
[ -x "$barm" ] || barm="$root/target/debug/barm"
work=$(mktemp -d)
trap 'rm -r "$work"' EXIT
if [ "$mode" = test ]; then cmd=test; else cmd=build; fi
# `barm test` builds and runs; we only want the C, so build via the test command's --emit-c.
BARM_CFLAGS= "$barm" "$cmd" "$path" --emit-c "$work/prog.c" -o "$work/native" >/dev/null 2>&1 || true
[ -s "$work/prog.c" ] || { echo "sanitize: failed to generate C for $path" >&2; exit 2; }
COPYFILE_DISABLE=1 tar -C "$work" -cf - prog.c | docker run --rm -i gcc:14 sh -c '
  mkdir /w && tar -C /w -xf - 2>/dev/null && cd /w &&
  gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -w prog.c -lm -o prog &&
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./prog'
