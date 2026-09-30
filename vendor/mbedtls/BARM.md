# mbedTLS, vendored

[Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 3.6.7 LTS gives Barm programs TLS: `fetch()` of `https:` URLs. It's licensed Apache-2.0 (see `LICENSE`). It's the only C code Barm vendors; the compiler itself still has no crate dependencies.

- Source: `mbedtls-3.6.7.tar.bz2` from the GitHub release, SHA-256 `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6` (as published in the release notes).
- Files: `include/` and `library/*.h` unchanged, and the `library/*.c` files that compile to code under our configuration (on some platform: `aesni.c` is x86-only). Every file is byte-for-byte upstream, so an update is a copy plus a diff.
- `barm_config.h` is our `MBEDTLS_CONFIG_FILE`: upstream's default configuration (`include/mbedtls/mbedtls_config.h`) edited with upstream's `scripts/config.py`.
  - **Off.** Legacy and unused parts:
    - DTLS, renegotiation, context serialization and the self-test.
    - PSK key exchanges, static-ECDH/RSA/DHE key exchanges, and TLS 1.3 pure-PSK mode (resumption keeps PSK-ephemeral).
    - Camellia, ARIA, DES, CCM, CMAC, NIST-KW, MD5, RIPEMD-160 and LMS.
    - PKCS#5/#7/#12, X.509 writing/CSR/CRL, the Brainpool/secp192/224/256k1 and Curve448 curves.
    - The socket and timing helpers, and PSA's file-backed key storage.
  - **On.** Speed: `MBEDTLS_GCM_LARGE_TABLE`, and the Arm SHA-256/512 instructions where present (`MBEDTLS_SHA{256,512}_USE_A64_CRYPTO_IF_PRESENT`). AES-NI and the Arm AES instructions are on by default.
- The build (`crates/barm/src/build.rs`) compiles these files, plus `runtime/tls.c` (the interface the fetch client uses), into a cached archive once per compiler. Only programs that call `fetch()` link it.

To update: download the new 3.6.x release, check its SHA-256 against the release notes, and copy `include/`, `library/*.h` and the same `library/*.c` files over these. Re-apply the `config.py` edits above to the new default configuration, then run `cargo test` (tests/fetch covers TLS).
