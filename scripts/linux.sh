#!/bin/sh
# Runs a command in Barm's Linux build container (scripts/linux/Dockerfile: Rust, a C compiler
# and WebKitGTK's JavaScriptCore), with this checkout at /barm and the build cached in volumes.
#   scripts/linux.sh                                   # the npm tests
#   scripts/linux.sh cargo test --release              # every test
#   scripts/linux.sh sh -c 'barm build x.barm && ./x'  # anything (target/release is on PATH)
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
docker build -q -t barm-linux "$root/scripts/linux" > /dev/null
[ $# -gt 0 ] || set -- cargo test --release --test npm
tty=""
[ -t 0 ] && tty="-t"
# Barm's own JavaScriptCore for Linux, when it's built (scripts/jsc/build.sh linux)
jsc=""
[ -f "$HOME/.cache/barm/jsc/linux-arm64/lib/libbarmjsc.a" ] && jsc="-v $HOME/.cache/barm/jsc/linux-arm64:/jsc -e BARM_JSC_DIR=/jsc"
exec docker run --rm -i $tty -v "$root:/barm" -v barm-linux-target:/target -v barm-linux-cargo:/usr/local/cargo/registry -v barm-linux-cache:/root/.cache/barm \
  $jsc -e CARGO_TARGET_DIR=/target -e PATH=/target/release:/usr/local/cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  -w /barm barm-linux "$@"
