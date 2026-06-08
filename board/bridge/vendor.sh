#!/usr/bin/env bash
# Vendor the TinyUSB + lwIP sources needed by the panda_bridge build into
# board/bridge/vendor/ (gitignored). Run once before building the bridge.
#
# Pulls from local clones (override with TINYUSB_SRC / LWIP_SRC env vars):
#   ~/github/tinyusb   (0.20.x — NCM class + dwc2 STM32H7 port)
#   ~/github/lwip       (2.2.x)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TINYUSB_SRC="${TINYUSB_SRC:-$HOME/github/tinyusb}"
LWIP_SRC="${LWIP_SRC:-$HOME/github/lwip}"
VENDOR="$HERE/vendor"

for d in "$TINYUSB_SRC/src" "$LWIP_SRC/src"; do
  [[ -d "$d" ]] || { echo "ERROR: missing $d (clone TinyUSB/lwIP or set TINYUSB_SRC/LWIP_SRC)" >&2; exit 1; }
done

rm -rf "$VENDOR"
mkdir -p "$VENDOR/tinyusb" "$VENDOR/lwip"

# TinyUSB: full src tree (we compile a subset in SConscript) + the net example's
# dhserver/dnserver helpers.
cp -R "$TINYUSB_SRC/src" "$VENDOR/tinyusb/src"
mkdir -p "$VENDOR/tinyusb/networking"
cp "$TINYUSB_SRC/lib/networking/dhserver.c" "$TINYUSB_SRC/lib/networking/dhserver.h" \
   "$TINYUSB_SRC/lib/networking/dnserver.c" "$TINYUSB_SRC/lib/networking/dnserver.h" \
   "$VENDOR/tinyusb/networking/"

# lwIP: full src tree (subset compiled in SConscript).
cp -R "$LWIP_SRC/src" "$VENDOR/lwip/src"

echo "Vendored:"
echo "  TinyUSB -> $VENDOR/tinyusb ($(cd "$TINYUSB_SRC" && git describe --tags 2>/dev/null || echo '?'))"
echo "  lwIP    -> $VENDOR/lwip    ($(cd "$LWIP_SRC" && git describe --tags 2>/dev/null || echo '?'))"
