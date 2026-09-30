# zlib inflate, vendored

The decompressor from [zlib](https://zlib.net) 1.3.1 (zlib license, see `LICENSE`) decodes `Content-Encoding: gzip` and `deflate` bodies for `fetch()`, as they arrive. The files are the inflate code (`inflate.c`, `inftrees.c`, `inffast.c` and their headers, `inffixed.h`), the checksums (`adler32.c`, `crc32.c`, `crc32.h`), `zutil.c`, and the headers `zlib.h`, `zconf.h`, `zutil.h` and `gzguts.h`.

They come from the release `zlib-1.3.1.tar.gz` (SHA-256 `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`), byte for byte. There's no compressor.

The build compiles them into the archive that fetch programs link, with BoringSSL (see `vendor/boringssl/BARM.md`). `runtime/codecs.c` calls them. zlib replaced the runtime's own inflate: it streams, and it decodes about 1.8 times as fast (3 to 4 times on small bodies, where its CRC-32 dominates).
