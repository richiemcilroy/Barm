# zstd decoder, vendored

The decompressor from [zstd](https://github.com/facebook/zstd) 1.5.7 (BSD, see `LICENSE`) decodes `Content-Encoding: zstd` bodies for `fetch()`. The files are:

- `lib/zstd.h` and `lib/zstd_errors.h`;
- `lib/decompress/` (with its x86-64 assembly);
- the parts of `lib/common/` decompression needs.

They come from the release `zstd-1.5.7.tar.gz` (SHA-256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`, as its `.sha256` says), byte for byte. There's no compressor.

The build compiles it into the archive that fetch programs link, with BoringSSL (see `vendor/boringssl/BARM.md`). `runtime/codecs.c` calls it.
