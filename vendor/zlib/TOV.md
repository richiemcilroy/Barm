# zlib, vendored

[zlib](https://zlib.net) 1.3.1 (zlib license, see `LICENSE`). Its deflate and inflate serve npm packages' `zlib` (`runtime/compress.c`). Its inflate also decodes `Content-Encoding: gzip` and `deflate` bodies that `fetch()` reads in chunks (`res.body`), as they arrive. Bodies read whole are decoded by libdeflate (`vendor/libdeflate`), which is faster but can't stream. The files are the deflate code (`deflate.c`, `trees.c` and their headers), the inflate code (`inflate.c`, `inftrees.c`, `inffast.c` and their headers, `inffixed.h`), the checksums (`adler32.c`, `crc32.c`, `crc32.h`), `zutil.c`, and the headers `zlib.h`, `zconf.h`, `zutil.h` and `gzguts.h`.

They come from the release `zlib-1.3.1.tar.gz` (SHA-256 `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`), byte for byte.

The build compiles them into the archive that fetch programs link, with BoringSSL (see `vendor/boringssl/TOV.md`). `runtime/codecs.c` and `runtime/compress.c` call them. zlib replaced the runtime's own inflate: it streams, and it decodes about 1.8 times as fast (3 to 4 times on small bodies, where its CRC-32 dominates).
