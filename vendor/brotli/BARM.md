# brotli decoder, vendored

The decoder from [brotli](https://github.com/google/brotli) 1.2.0 (MIT, see `LICENSE`) decodes `Content-Encoding: br` bodies for `fetch()`. `c/common`, `c/dec` and the decoder's public headers come from the `v1.2.0` tag (GitHub's source archive, SHA-256 `816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec`), byte for byte. There's no encoder.

The build compiles it into the archive that fetch programs link, with BoringSSL (see `vendor/boringssl/BARM.md`). `runtime/codecs.c` calls it.
