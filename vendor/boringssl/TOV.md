# BoringSSL, vendored

[BoringSSL](https://boringssl.googlesource.com/boringssl) 0.20260929.0 gives Tov programs TLS: `fetch()` of `https:` URLs. Chrome and Bun use it too. It's licensed Apache-2.0 (older files OpenSSL/ISC; see `LICENSE`). It's the only C/C++ code Tov vendors; the compiler itself still has no crate dependencies.

- Source: `boringssl-0.20260929.0.tar.gz` from the GitHub release, SHA-256 `04da9ba0664e0a7f028e961c38d604f2cc6dac852a84ca0e51dc1fa051d4c8fe` (GitHub's published digest for the asset).
- Files: exactly those `gen/sources.json` lists for the `bcm`, `crypto` and `ssl` targets:
  - their sources, public and internal headers, and `.inc` files;
  - their pre-generated assembly for Apple and Linux (x86_64, aarch64 and 32-bit Arm), so no Perl is needed. The Windows assembly isn't included.
  
  Every file is byte-for-byte upstream.
- The build (`crates/tov/src/build.rs`) compiles these files, plus `runtime/tls.c`, into a cached archive, once per compiler:
  - C++17 without exceptions or RTTI, `-O2 -fno-strict-aliasing -fvisibility=hidden`, as BoringSSL's CMake does.
  - Only programs that call `fetch()` link it, along with the C++ runtime (`-lc++` on macOS, `-lstdc++` on Linux) for a few helpers.

To update: download the new release, check its digest, and replace these files with the ones the new `gen/sources.json` lists for the same targets (same filters). Then run `cargo test`; `tests/fetch/tls.tov` covers TLS and compares with Bun.
