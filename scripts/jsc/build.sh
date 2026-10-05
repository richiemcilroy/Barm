#!/bin/sh
# Builds Tov's JavaScriptCore: WebKit's JSCOnly port at a pinned release, with Tov's patches
# (scripts/jsc/patches), as one static library and its headers in
#   ~/.cache/tov/jsc/<os>-<arch>/{lib/libtovjsc.a, include/JavaScriptCore/*.h, VERSION}
# which `tov build` links programs that import npm packages with (TOV_JSC=system uses the
# system's instead). On macOS it builds here; `scripts/jsc/build.sh linux` builds Linux's in a
# container (scripts/jsc/Dockerfile.linux).
set -eu
TAG=webkitgtk-2.54.1
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cache=${TOV_JSC_CACHE:-$HOME/.cache/tov/jsc}
mkdir -p "$cache"

if [ "${1:-}" = linux ]; then
    docker build -q -t tov-jsc-linux -f "$here/Dockerfile.linux" "$here" > /dev/null
    exec docker run --rm -v "$root:/tov" -v "$cache:/jsc" -e TOV_JSC_CACHE=/jsc tov-jsc-linux sh /tov/scripts/jsc/build.sh
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
# (ThinLTO on macOS, whose packaging below merges the bitcode into machine code; Linux's doesn't
# yet, so its objects stay machine code)
lto="-DLTO_MODE="
[ "$os" = darwin ] && lto="-DLTO_MODE=thin"
# (ThinLTO: the objects are bitcode, optimized across each other when they're merged below, so
# a program's own link stays an ordinary one)
cmake -S "$src" -B "$build" -G Ninja -DPORT=JSCOnly -DCMAKE_BUILD_TYPE=Release -DENABLE_STATIC_JSC=ON \
    -DUSE_THIN_ARCHIVES=OFF -DENABLE_FTL_JIT=ON -DDEVELOPER_MODE=OFF -DENABLE_API_TESTS=OFF -DENABLE_TOOLS=OFF \
    -DUSE_CXX_STDLIB_ASSERTIONS=OFF $lto -DUSE_LD_LLD=OFF \
    -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_CXX_FLAGS="$flags -DTOV_JSC_CACHE_VERSION=${version}u" > "$cache/configure-$os-$arch.log"
nice -n 10 cmake --build "$build" --target JavaScriptCore JavaScriptCoreJIT > "$cache/build-$os-$arch.log"

# one archive: JavaScriptCore, its JIT's objects (an object library), WTF and bmalloc
rm -rf "$out.tmp"
mkdir -p "$out.tmp/lib" "$out.tmp/include/JavaScriptCore"
jit=$(find "$build/Source/JavaScriptCore/CMakeFiles/JavaScriptCoreJIT.dir" -name '*.o')
if [ "$os" = darwin ]; then
    # The interpreters (LLInt, IPInt) are assembly whose handlers are found at offsets from each
    # other: a program linked with -dead_strip mustn't drop those no symbol names, as the object
    # (MH_SUBSECTIONS_VIA_SYMBOLS) would let it. Without the flag the section is kept whole. It's
    # compiled to machine code on its own, outside the link-time optimization.
    llint="$out.tmp/LowLevelInterpreter.o"
    clang -c -O3 -x ir "$build/Source/JavaScriptCore/CMakeFiles/LowLevelInterpreterLib.dir/llint/LowLevelInterpreter.cpp.o" -o "$llint"
    python3 -c 'import struct, sys
b = bytearray(open(sys.argv[1], "rb").read())
assert struct.unpack_from("<I", b, 0)[0] == 0xfeedfacf
struct.pack_into("<I", b, 24, struct.unpack_from("<I", b, 24)[0] & ~0x2000)
open(sys.argv[1], "wb").write(b)' "$llint"
    cp "$build/lib/libJavaScriptCore.a" "$out.tmp/libJavaScriptCore.a"
    ar -d "$out.tmp/libJavaScriptCore.a" LowLevelInterpreter.cpp.o
    # the rest, optimized as one program (ThinLTO) into one object of machine code, which a
    # program's link takes in atom by atom as from any object: all of the engine, and what it
    # uses of WTF and bmalloc (which hold alternatives for other platforms), keeping what the
    # interpreters, outside it, call (else they're the engine's own, and unused, to the optimizer)
    nm -u "$llint" | sed 's/^/-Wl,-u,/' > "$out.tmp/llint-uses"
    clang -r -nostdlib -flto=thin -O3 -Wl,-keep_private_externs @"$out.tmp/llint-uses" -Wl,-force_load,"$out.tmp/libJavaScriptCore.a" $jit "$build/lib/libWTF.a" "$build/lib/libbmalloc.a" \
        -Wl,-cache_path_lto,"$cache/lto-cache-$os-$arch" -o "$out.tmp/engine.o"
    libtool -static -o "$out.tmp/lib/libtovjsc.a" "$out.tmp/engine.o" "$llint" 2> /dev/null
    rm "$llint" "$out.tmp/llint-uses" "$out.tmp/libJavaScriptCore.a" "$out.tmp/engine.o"
    headers="$build/JavaScriptCore.framework/Headers"
else
    {
        echo "CREATE $out.tmp/lib/libtovjsc.a"
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
echo "built $out ($(du -sh "$out/lib/libtovjsc.a" | cut -f1))"
