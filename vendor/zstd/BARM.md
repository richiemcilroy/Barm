# zstd, vendored

[zstd](https://github.com/facebook/zstd) 1.5.7 (BSD, see `LICENSE`). The decompressor decodes `Content-Encoding: zstd` bodies for `fetch()`. The compressor and decompressor both serve npm packages' `zlib` (`runtime/compress.c`). The files are:

- `lib/zstd.h` and `lib/zstd_errors.h`;
- `lib/decompress/` (with its x86-64 assembly);
- `lib/compress/`;
- the parts of `lib/common/` both need (`pool.c` and `threading.c` compile to the single-threaded versions).

They come from the release `zstd-1.5.7.tar.gz` (SHA-256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`, as its `.sha256` says), byte for byte.

The build compiles it into the archive that fetch programs link, with BoringSSL (see `vendor/boringssl/BARM.md`). `runtime/codecs.c` and `runtime/compress.c` call it.
