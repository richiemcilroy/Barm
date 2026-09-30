# libdeflate decompressor, vendored

The decompressor from [libdeflate](https://github.com/ebiggers/libdeflate) 1.24 (MIT, see `COPYING`) decodes `Content-Encoding: gzip` and `deflate` bodies that `fetch()` reads whole (`text()`, `json()`, `bytes()` and the like), at once, when they have all arrived: about twice as fast as zlib. It can't decode a stream in pieces, so bodies read in chunks (`res.body`) go through zlib (`vendor/zlib`).

The files are `libdeflate.h`, `common_defs.h`, and from `lib/`: the decompressors (`deflate_decompress.c` with `decompress_template.h`, `gzip_decompress.c`, `zlib_decompress.c`), the checksums (`adler32.c`, `crc32.c`) and what they include, `utils.c`, and the CPU feature detection and SIMD code in `lib/arm/` and `lib/x86/`. There's no compressor.

They come from the release tag `v1.24` (`libdeflate-1.24.tar.gz` from GitHub, SHA-256 `ad8d3723d0065c4723ab738be9723f2ff1cb0f1571e8bfcf0301ff9661f475e8`), byte for byte.

The build compiles them into the archive that fetch programs link, with BoringSSL (see `vendor/boringssl/BARM.md`). `runtime/codecs.c` calls them.
