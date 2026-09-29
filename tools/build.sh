#!/usr/bin/env bash
# Build this project from Git Bash / MSYS.
#
# Why this script exists
# ----------------------
# `idf.py` refuses to run under MSYS/MinGW. Its entry point is:
#
#     if 'MSYSTEM' in os.environ:
#         print_warning('MSys/Mingw is no longer supported...')
#     else:
#         main()
#
# Note there is no `main()` call in the MSYS branch - the process prints one
# warning and exits 0 without doing anything. Git Bash always exports
# MSYSTEM=MINGW64, so a plain `idf.py build` silently builds nothing.
#
# This script strips MSYSTEM and calls idf.main() directly.
#
# Usage:
#   tools/build.sh            # build
#   tools/build.sh menuconfig
#   tools/build.sh -p COM8 flash monitor
#
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Keep this feature branch self-contained.  The dashboard needs partitions.csv,
# while main deliberately uses ESP-IDF's stock partition layout.  ESP-IDF does
# not switch ignored sdkconfig/build files when Git switches branches, so using
# branch-specific paths prevents one branch's generated state from breaking the
# other one.  Override either path only for an intentional custom build.
BUILD_DIR="${ESP_BUILD_DIR:-$PROJECT_DIR/build-dev}"
SDKCONFIG_FILE="${ESP_SDKCONFIG:-$PROJECT_DIR/sdkconfig.dev}"

# --- Adjust these two if your ESP-IDF installation lives elsewhere ---------
export IDF_PATH="${IDF_PATH:-F:/ESP32/Espressif/v5.5.5/esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-C:/Espressif/tools}"

IDF_VERSION_DIR="$(basename "$(dirname "$IDF_PATH")")"   # .../v5.5.5/esp-idf -> v5.5.5
PYTHON_ENV="$IDF_TOOLS_PATH/python/$IDF_VERSION_DIR/venv"
IDF_PYTHON="$PYTHON_ENV/Scripts/python.exe"

# Fall back to whatever single python env the installer left behind.
if [[ ! -x "$IDF_PYTHON" ]]; then
    for candidate in "$IDF_TOOLS_PATH"/python/*/venv/Scripts/python.exe; do
        if [[ -x "$candidate" ]]; then
            IDF_PYTHON="$candidate"
            PYTHON_ENV="$(dirname "$(dirname "$candidate")")"
            break
        fi
    done
fi
# --------------------------------------------------------------------------

if [[ ! -x "$IDF_PYTHON" ]]; then
    echo "ESP-IDF python not found at: $IDF_PYTHON" >&2
    echo "Set IDF_PATH / IDF_TOOLS_PATH to match your installation." >&2
    exit 1
fi

# Bash uses ':' between PATH entries, which would split a Windows path such as
# C:/Espressif/tools at its drive-letter colon. Convert only the paths that are
# appended to PATH; IDF_PATH and IDF_TOOLS_PATH stay in Windows form for
# Windows Python.
IDF_TOOLS_PATH_BASH="$(cygpath -u "$IDF_TOOLS_PATH")"

# Put the cross toolchain, cmake and ninja on PATH.
for candidate in \
    "$IDF_TOOLS_PATH_BASH"/xtensa-esp-elf/*/xtensa-esp-elf/bin \
    "$IDF_TOOLS_PATH_BASH"/cmake/*/bin \
    "$IDF_TOOLS_PATH_BASH"/ninja/* ; do
    if [[ -d "$candidate" ]]; then
        PATH="$candidate:$PATH"
    fi
done
export PATH

export IDF_PYTHON_ENV_PATH="$PYTHON_ENV"

# The wrapper lives inside the isolated build directory (a disposable directory) so no cleanup is
# needed - plain `rm` is unreliable in some sandboxed Git Bash environments.
mkdir -p "$BUILD_DIR"
WRAPPER="$BUILD_DIR/idfrun_wrapper.py"

cat > "$WRAPPER" <<'PYEOF'
"""Call idf.main() with MSYSTEM removed so the MSYS guard is bypassed."""
import os
import sys

os.environ.pop("MSYSTEM", None)
os.environ.pop("MSYSCON", None)
sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "tools"))
sys.argv = ["idf.py"] + sys.argv[1:]

import idf  # noqa: E402

idf.main()
PYEOF

cd "$PROJECT_DIR"
if [[ $# -eq 0 ]]; then
    set -- build
fi

"$IDF_PYTHON" "$WRAPPER" -B "$BUILD_DIR" -D "SDKCONFIG=$SDKCONFIG_FILE" "$@"
