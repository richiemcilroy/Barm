#!/bin/sh
# Runs the HTTP benchmark on Linux (epoll) in Docker: Barm, Rust (axum) and Bun.
# The Barm server is emitted as C and compiled in the container; the Rust server is
# cross-compiled with zig; Bun comes from the oven/bun image.
#   bench/http/linux.sh [run.py args...]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
bin="$here/out/linux"
mkdir -p "$bin"
cargo build --release -q --manifest-path "$root/Cargo.toml"
"$root/target/release/barm" build "$here/server.barm.ts" --emit-c "$bin/barm-server.c" -o "$here/out/barm-server" > /dev/null
linker="$bin/zig-linker"
cat > "$linker" <<'PY'
#!/usr/bin/env python3
import os, sys
args = [a for a in sys.argv[1:] if a != "-Wl,--fix-cortex-a53-843419"]
os.execvp("zig", ["zig", "cc", "-target", "aarch64-linux-gnu.2.28"] + args)
PY
chmod +x "$linker"
CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER="$linker" cargo build --release -q --offline --target aarch64-unknown-linux-gnu --manifest-path "$here/rust/Cargo.toml"
cp "$here/rust/target/aarch64-unknown-linux-gnu/release/server" "$bin/rust-server"
[ -x "$bin/bun" ] || docker run --rm -v "$bin:/out" oven/bun:1.4.0 cp /usr/local/bin/bun /out/bun
docker run --rm -v "$root:/barm" -w /barm/bench/http -e BIN_DIR=/barm/bench/http/out/linux gcc:14 sh -c '
  set -e
  gcc -O2 -o $BIN_DIR/load load.c -lpthread
  gcc -std=gnu11 -O2 -w -ffp-contract=off -o $BIN_DIR/barm-server $BIN_DIR/barm-server.c -lm -lpthread
  python3 run.py --no-build --only barm,rust,rust-tpc,bun "$@"
' sh "$@"
