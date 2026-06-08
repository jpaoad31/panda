#!/usr/bin/env bash
# Build panda firmware on macOS with a fully self-contained, FOLDER-LOCAL env.
#
# Everything it creates (the venv, the Python interpreter uv downloads, and uv's
# cache) lives under the panda repo — nothing touches your global environment.
#
# Usage:
#   board/bridge/macos_build.sh            # build everything (default scons target)
#   board/bridge/macos_build.sh -c         # clean
#   board/bridge/macos_build.sh -j8 <args> # pass-through scons args
#
# Why this script exists (two macOS gotchas that the stock docs miss):
#   1. Homebrew's `arm-none-eabi-gcc` is a BARE compiler with no C library, so it
#      can't find newlib's <stdint.h>. Use the ARM GNU Toolchain in /Applications
#      (bundles newlib) instead — that's what this script puts on PATH.
#   2. board/crypto/sign.py runs via `#!/usr/bin/env python3`, so the venv must be
#      FIRST on PATH or it picks up system python3 (which lacks pycryptodome).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

# Keep uv's venv, downloaded Python, and cache inside the repo.
export UV_PROJECT_ENVIRONMENT="$ROOT/.venv"
export UV_CACHE_DIR="$ROOT/.uv-cache"
export UV_PYTHON_INSTALL_DIR="$ROOT/.uv-python"
export UV_PYTHON_PREFERENCE=only-managed

# ARM GNU Toolchain (newlib-equipped). Pick the highest version present.
ARM_BIN="$(ls -d /Applications/ArmGNUToolchain/*/arm-none-eabi/bin 2>/dev/null | sort -V | tail -1 || true)"
if [[ -z "${ARM_BIN}" ]]; then
  echo "ERROR: ARM GNU Toolchain not found under /Applications/ArmGNUToolchain." >&2
  echo "Install ARM's toolchain (e.g. 'brew install --cask gcc-arm-embedded')." >&2
  echo "Do NOT rely on the bare 'arm-none-eabi-gcc' Homebrew formula — it lacks newlib." >&2
  exit 1
fi

# First run: build the local venv (Python 3.12 + opendbc) + add scons + pycryptodome.
if [[ ! -x "$ROOT/.venv/bin/scons" ]]; then
  command -v uv >/dev/null || { echo "ERROR: 'uv' not found. Install: brew install uv" >&2; exit 1; }
  echo ">> Creating local venv (Python 3.12, opendbc) under $ROOT/.venv ..."
  uv sync --python 3.12
  echo ">> Installing scons + pycryptodome into the venv ..."
  uv pip install scons pycryptodome
fi

# venv/bin FIRST (for sign.py's shebang), then the ARM toolchain.
export PATH="$ROOT/.venv/bin:$ARM_BIN:$PATH"

echo ">> arm-none-eabi-gcc: $(command -v arm-none-eabi-gcc)"
echo ">> scons:            $(command -v scons)"
exec scons "$@"
