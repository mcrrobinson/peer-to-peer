#!/usr/bin/env bash
# Builds libopus 1.5.2 with DRED (Deep REDundancy) into build/third_party/opus.
# Debian's libopus is compiled without DRED; CMake picks this build up
# automatically when it exists (or pass -DP2P_OPUS_PREFIX=<dir>).
set -euo pipefail
VER=1.5.2
SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
D="$ROOT/build/third_party"
mkdir -p "$D/src"
cd "$D/src"
[ -f "opus-$VER.tar.gz" ] || curl -fsSL -o "opus-$VER.tar.gz" "https://downloads.xiph.org/releases/opus/opus-$VER.tar.gz"
echo "$SHA256  opus-$VER.tar.gz" | sha256sum -c -
rm -rf "opus-$VER" && tar xzf "opus-$VER.tar.gz"
cd "opus-$VER"
./configure --prefix="$D/opus" --enable-dred --disable-doc --disable-extra-programs \
    --enable-static --disable-shared --with-pic CFLAGS="-O2"
make -j"$(nproc)"
make install
echo "libopus with DRED installed to $D/opus — re-run: cmake --preset dev"
