#!/bin/sh
# Builds Barm's JavaScriptCore: WebKit's JSCOnly port at a pinned release, with Barm's patches
# (scripts/jsc/patches), as one static library and its headers in
#   ~/.cache/barm/jsc/<os>-<arch>/{lib/libbarmjsc.a, include/JavaScriptCore/*.h, VERSION}
# which `barm build` links programs that import npm packages with (BARM_JSC=system uses the
# system's instead). On macOS it builds here; `scripts/jsc/build.sh linux` builds Linux's in a
# container (scripts/jsc/Dockerfile.linux).
set -eu
TAG=webkitgtk-2.54.1
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cache=${BARM_JSC_CACHE:-$HOME/.cache/barm/jsc}
mkdir -p "$cache"

if [ "${1:-}" = linux ]; then
    docker build -q -t barm-jsc-linux -f "$here/Dockerfile.linux" "$here" > /dev/null
    exec docker run --rm -v "$root:/barm" -v "$cache:/jsc" -e BARM_JSC_CACHE=/jsc barm-jsc-linux sh /barm/scripts/jsc/build.sh
fi

src="$cache/src-$TAG"
if [ ! -d "$src/Source/JavaScriptCore" ]; then
    rm -rf "$src.tmp"
    git clone --quiet --filter=blob:none --no-checkout --depth 1 --branch "$TAG" https://github.com/WebKit/WebKit.git "$src.tmp"
    git -C "$src.tmp" sparse-checkout init --cone
    git -C "$src.tmp" sparse-checkout set Source/JavaScriptCore Source/WTF Source/bmalloc Source/cmake \
        Source/ThirdParty/capstone Source/ThirdParty/unifdef Source/ThirdParty/gtest Tools/Scripts
    git -C "$src.tmp" checkout --quiet
    mv "$src.tmp" "$src"
fi
# the patches, on a clean tree
git -C "$src" checkout --quiet -- .
for p in "$here"/patches/*.patch; do
    [ -f "$p" ] && git -C "$src" apply "$p"
done
cp "$here"/patches/*.cpp "$here"/patches/*.h "$src/Source/JavaScriptCore/API/" 2>/dev/null || true

# what the bytecode cache's format is versioned by (the build's, not the program binary's)
version=$( (echo "$TAG"; cat "$here"/patches/* "$here"/include/*/*) | cksum | cut -d' ' -f1)
os=$(uname -s | tr A-Z a-z)
arch=$(uname -m)
[ "$arch" = aarch64 ] && arch=arm64
build="$cache/build-$os-$arch"
# (macOS: libpthread's private header, for the engine's fast thread-local slots: include/)
flags="-ffunction-sections -fdata-sections"
[ "$os" = darwin ] && flags="$flags -I$here/include"
out="$cache/$os-$arch"
cmake -S "$src" -B "$build" -G Ninja -DPORT=JSCOnly -DCMAKE_BUILD_TYPE=Release -DENABLE_STATIC_JSC=ON \
    -DUSE_THIN_ARCHIVES=OFF -DENABLE_FTL_JIT=ON -DDEVELOPER_MODE=OFF -DENABLE_API_TESTS=OFF -DENABLE_TOOLS=OFF \
    -DUSE_CXX_STDLIB_ASSERTIONS=OFF \
    -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_CXX_FLAGS="$flags -DBARM_JSC_CACHE_VERSION=${version}u" > "$cache/configure-$os-$arch.log"
nice -n 10 cmake --build "$build" --target JavaScriptCore JavaScriptCoreJIT > "$cache/build-$os-$arch.log"

# one archive: JavaScriptCore, its JIT's objects (an object library), WTF and bmalloc
rm -rf "$out.tmp"
mkdir -p "$out.tmp/lib" "$out.tmp/include/JavaScriptCore"
jit=$(find "$build/Source/JavaScriptCore/CMakeFiles/JavaScriptCoreJIT.dir" -name '*.o')
if [ "$os" = darwin ]; then
    # The interpreters (LLInt, IPInt) are assembly whose handlers are found at offsets from each
    # other: a program linked with -dead_strip mustn't drop those no symbol names, as the object
    # (MH_SUBSECTIONS_VIA_SYMBOLS) would let it. Without the flag the section is kept whole.
    llint="$out.tmp/LowLevelInterpreter.cpp.o"
    cp "$build/Source/JavaScriptCore/CMakeFiles/LowLevelInterpreterLib.dir/llint/LowLevelInterpreter.cpp.o" "$llint"
    python3 -c 'import struct, sys
b = bytearray(open(sys.argv[1], "rb").read())
assert struct.unpack_from("<I", b, 0)[0] == 0xfeedfacf
struct.pack_into("<I", b, 24, struct.unpack_from("<I", b, 24)[0] & ~0x2000)
open(sys.argv[1], "wb").write(b)' "$llint"
    cp "$build/lib/libJavaScriptCore.a" "$out.tmp/libJavaScriptCore.a"
    ar -d "$out.tmp/libJavaScriptCore.a" LowLevelInterpreter.cpp.o
    libtool -static -o "$out.tmp/lib/libbarmjsc.a" "$out.tmp/libJavaScriptCore.a" "$build/lib/libWTF.a" "$build/lib/libbmalloc.a" "$llint" $jit 2> /dev/null
    rm "$llint" "$out.tmp/libJavaScriptCore.a"
    headers="$build/JavaScriptCore.framework/Headers"
else
    {
        echo "CREATE $out.tmp/lib/libbarmjsc.a"
        for a in libJavaScriptCore.a libWTF.a libbmalloc.a; do echo "ADDLIB $build/lib/$a"; done
        for o in $jit; do echo "ADDMOD $o"; done
        echo SAVE
        echo END
    } | ar -M
    headers="$build/JavaScriptCore/Headers/JavaScriptCore"
fi
cp "$headers"/*.h "$out.tmp/include/JavaScriptCore/"
echo "$TAG $version" > "$out.tmp/VERSION"
rm -rf "$out"
mv "$out.tmp" "$out"
echo "built $out ($(du -sh "$out/lib/libbarmjsc.a" | cut -f1))"
