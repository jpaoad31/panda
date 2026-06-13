#!/usr/bin/env bash
# Build the panda firmware on a Raspberry Pi (Linux), mirroring macos_build.sh's
# key trick: put the venv bin FIRST on PATH so board/crypto/sign.py's
# `#!/usr/bin/env python3` shebang picks up pycryptodome (not system python3), and
# point PYTHONPATH at the comma opendbc clone so `import opendbc` resolves.
#
# One-time setup on the Pi:
#   sudo apt install -y gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
#   python3 -m venv ~/pandaenv && ~/pandaenv/bin/pip install scons pycryptodome numpy pycapnp
#   git clone --depth 1 https://github.com/commaai/opendbc ~/opendbc
#
# Usage (from anywhere):  board/bridge/tools/pi_build.sh board/obj/panda_bridge.bin.signed board/obj/bootstub.panda_bridge.bin
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/../../.."   # -> repo root
export PATH="$HOME/pandaenv/bin:$PATH"
export PYTHONPATH="$HOME/opendbc"
exec scons "$@"
