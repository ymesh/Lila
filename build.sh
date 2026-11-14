#!/usr/bin/env bash
# Configures if needed, then builds every target. Usage: ./build.sh [build dir] [config], defaults build RelWithDebInfo.
# Linux is experimental and untested: Qt comes from vcpkg and Python from scripts/fetch-python.sh, never from the distro.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
DIR="${1:-build}"
CONFIG="${2:-RelWithDebInfo}"

if [ -z "${VCPKG_ROOT:-}" ]; then
    echo "[failed] Set VCPKG_ROOT to a clone of github.com/microsoft/vcpkg (after running its bootstrap-vcpkg.sh)."
    exit 1
fi
if [ -z "${LILA_PYTHON_DIR:-}" ]; then
    LILA_PYTHON_DIR="$("$ROOT/scripts/fetch-python.sh")"
fi
export LILA_PYTHON_DIR
export PATH="$LILA_PYTHON_DIR/bin:$PATH"

# LILA_PRESET: the configure preset, default linux (linux-clang: Clang for Lila and its ports, into build-clang).
# LILA_VCPKG_INSTALLED_DIR: a vcpkg_installed of its own, for a second Linux (a distro container, another compiler) whose
# packages must not overwrite the default one; both use the x64-linux triplet.
EXTRA=()
if [ -n "${LILA_VCPKG_INSTALLED_DIR:-}" ]; then
    EXTRA+=("-DVCPKG_INSTALLED_DIR=$LILA_VCPKG_INSTALLED_DIR")
fi
if [ ! -f "$ROOT/$DIR/build.ninja" ]; then
    # The USD port only knows a Windows Python layout.
    cmake --preset "${LILA_PRESET:-linux}" -S "$ROOT" -B "$ROOT/$DIR" -DLILA_USD=OFF "${EXTRA[@]}"
fi
cmake --build "$ROOT/$DIR" --config "$CONFIG"
