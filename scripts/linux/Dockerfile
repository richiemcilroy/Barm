# Tov's Linux build and tests (scripts/linux.sh): Rust, a C compiler, and for npm packages
# JavaScriptCore as WebKitGTK ships it (or ICU, for Tov's own: scripts/jsc).
FROM rust:1-trixie
RUN apt-get update && apt-get install -y --no-install-recommends libjavascriptcoregtk-4.1-dev libicu-dev pkg-config python3 && apt-get clean
