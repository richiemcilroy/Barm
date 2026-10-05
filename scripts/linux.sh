#!/bin/sh
# Runs a command in Tov's Linux build container (scripts/linux/Dockerfile: Rust, a C compiler
# and WebKitGTK's JavaScriptCore), with this checkout at /tov and the build cached in volumes.
#   scripts/linux.sh                                   # the npm tests
#   scripts/linux.sh cargo test --release              # every test
#   scripts/linux.sh sh -c 'tov build x.tov && ./x'  # anything (target/release is on PATH)
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
docker build -q -t tov-linux "$root/scripts/linux" > /dev/null
[ $# -gt 0 ] || set -- cargo test --release --test npm
tty=""
[ -t 0 ] && tty="-t"
# Tov's own JavaScriptCore for Linux, when it's built (scripts/jsc/build.sh linux)
jsc=""
[ -f "$HOME/.cache/tov/jsc/linux-arm64/lib/libtovjsc.a" ] && jsc="-v $HOME/.cache/tov/jsc/linux-arm64:/jsc -e TOV_JSC_DIR=/jsc"
exec docker run --rm -i $tty -v "$root:/tov" -v tov-linux-target:/target -v tov-linux-cargo:/usr/local/cargo/registry -v tov-linux-cache:/root/.cache/tov \
  $jsc -e CARGO_TARGET_DIR=/target -e PATH=/target/release:/usr/local/cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  -w /tov tov-linux "$@"
