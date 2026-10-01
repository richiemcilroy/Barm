# brotli, vendored

[brotli](https://github.com/google/brotli) 1.2.0 (MIT, see `LICENSE`). The decoder decodes `Content-Encoding: br` bodies for `fetch()`. The encoder and decoder both serve npm packages' `zlib` (`runtime/compress.c`). `c/common`, `c/dec`, `c/enc` and the public headers come from the `v1.2.0` tag (GitHub's source archive, SHA-256 `816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec`), byte for byte. The one file left out is `c/enc/static_init_lazy.cc`, which brotli builds only when its tables are initialized lazily.

The build compiles it into the archive that fetch programs link, with BoringSSL (see `vendor/boringssl/BARM.md`). `runtime/codecs.c` and `runtime/compress.c` call it.
