#!/usr/bin/env bash
# Builds libopus 1.6.1 with DRED (Deep REDundancy) into build/third_party/opus.
# Distro/Homebrew libopus is compiled without DRED; CMake picks this build up
# automatically when it exists (or pass -DP2P_OPUS_PREFIX=<dir>).
#
# Applies the upstream fixes in tools/patches/opus-$VER/ (DRED encoder/parser
# fixes and a bound on DRED latents decoded from untrusted packets) until a
# release includes them.
#
# Not enabled: --enable-osce (LACE/NoLACE/BWE only act on 20 ms SILK-wideband
# frames; this app sends 10 ms hybrid fullband) and --enable-qext (unused, and
# extra packet-parsing code facing the peer).
set -euo pipefail
VER=1.6.1
SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
D="$ROOT/build/third_party"
mkdir -p "$D/src"
cd "$D/src"
[ -f "opus-$VER.tar.gz" ] || curl -fsSL -o "opus-$VER.tar.gz" "https://downloads.xiph.org/releases/opus/opus-$VER.tar.gz"
if command -v sha256sum >/dev/null; then
    echo "$SHA256  opus-$VER.tar.gz" | sha256sum -c -
else
    echo "$SHA256  opus-$VER.tar.gz" | shasum -a 256 -c -
fi
rm -rf "opus-$VER" && tar xzf "opus-$VER.tar.gz"
cd "opus-$VER"
for p in "$ROOT"/tools/patches/opus-$VER/*.patch; do
    [ -e "$p" ] || continue
    echo "applying $(basename "$p")"
    patch -p1 -s < "$p"
done
./configure --prefix="$D/opus" --enable-dred --disable-doc --disable-extra-programs \
    --enable-static --disable-shared --with-pic CFLAGS="-O2"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make install
# DRED's wire format is versioned separately from libopus; peers only send
# DRED to each other when these match (see media::MediaCaps::audioDredRev).
REV=$(sed -n 's/^#define DRED_EXPERIMENTAL_VERSION \([0-9]*\).*/\1/p' dnn/dred_config.h)
[ -n "$REV" ] || { echo "DRED_EXPERIMENTAL_VERSION not found" >&2; exit 1; }
echo "#define P2P_OPUS_DRED_REV $REV" > "$D/opus/include/opus/p2p_dred_rev.h"
echo "libopus $VER with DRED (bitstream rev $REV) installed to $D/opus"
echo "re-run: cmake --preset dev -U 'OPUS*' -U '__pkg_config_checked_OPUS'"
