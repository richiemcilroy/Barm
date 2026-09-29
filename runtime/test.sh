#!/bin/sh
# Runs the Barm runtime tests:
#   1. warning-free compile of barm.c (-std=c11 -Wall -Wextra -Werror)
#   2. test_runtime.c at -O2
#   3. test_runtime.c under AddressSanitizer + UndefinedBehaviorSanitizer (+ LeakSanitizer on Linux)
#   4. for each build: `test_runtime trap` must print the trap to stderr, flush stdout, exit 101
#
# Usage: runtime/test.sh            (CC=gcc runtime/test.sh to pick a compiler)
#        runtime/test.sh --docker   (run inside the gcc:14 image, e.g. where ASan is unavailable)
set -eu
cd "$(dirname "$0")"

if [ "${1:-}" = "--docker" ]; then
    # stream the sources in (bind mounts can serve stale files on Docker Desktop)
    COPYFILE_DISABLE=1 tar -cf - barm.h barm.c test_runtime.c test.sh |
        docker run --rm -i gcc:14 sh -c 'mkdir /rt && tar -C /rt -xf - 2>/dev/null && sh /rt/test.sh'
    exit $?
fi

CC=${CC:-cc}
OUT=$(mktemp -d "${TMPDIR:-/tmp}/barm-runtime-test.XXXXXX")
trap 'rm -rf "$OUT"' EXIT INT TERM
WARN="-std=c11 -Wall -Wextra -Werror"

check_trap_exit() { # $1 = binary
    set +e
    "$1" trap >"$OUT/trap.out" 2>"$OUT/trap.err"
    st=$?
    set -e
    if [ "$st" -ne 101 ] || ! grep -qx "flushed before trap" "$OUT/trap.out" ||
        ! grep -qx "trap: index 5 out of bounds for length 0" "$OUT/trap.err" ||
        ! grep -qx "  at cli.barm:1:1" "$OUT/trap.err"; then
        echo "FAIL: trap exit check (status $st)"
        cat "$OUT/trap.out" "$OUT/trap.err"
        exit 1
    fi
    echo "trap exit status 101: ok"
}

# ASan hangs at startup on some macOS betas; probe with a watchdog before relying on it.
asan_works() ( # subshell: keeps the shell's "Killed" job notice out of the output
    printf 'int main(void){return 0;}\n' >"$OUT/probe.c"
    $CC -fsanitize=address "$OUT/probe.c" -o "$OUT/probe" 2>/dev/null || exit 1
    "$OUT/probe" &
    pid=$!
    i=0
    while kill -0 "$pid" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -gt 50 ]; then
            kill -9 "$pid" 2>/dev/null || true
            exit 1
        fi
        sleep 0.2
    done
    wait "$pid"
)

echo "== $($CC --version | head -n 1)"
echo "== compile barm.c"
$CC $WARN -O2 -c barm.c -o "$OUT/barm.o"

echo "== -O2"
$CC $WARN -O2 test_runtime.c -o "$OUT/t_o2" -lm
"$OUT/t_o2"
check_trap_exit "$OUT/t_o2"

if asan_works 2>/dev/null; then
    SAN=address,undefined
else
    SAN=undefined
    echo "warning: AddressSanitizer does not run here; using UBSan only (try: runtime/test.sh --docker)"
fi
echo "== -fsanitize=$SAN"
$CC -std=c11 -g -O1 -fsanitize=$SAN -fno-sanitize-recover=all -fno-omit-frame-pointer test_runtime.c -o "$OUT/t_san" -lm
if [ "$(uname)" = Linux ] && [ "$SAN" != undefined ]; then
    export ASAN_OPTIONS=detect_leaks=1
fi
"$OUT/t_san"
check_trap_exit "$OUT/t_san"
echo "all runtime tests passed"
