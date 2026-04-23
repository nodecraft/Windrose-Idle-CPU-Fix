#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build WindroseIdleCpuFix.dll and package build/WindroseIdleCpuFix.zip
# using clang-cl + xwin on Linux. Idempotent: re-running only does the work
# that's actually needed.
#
# Prereqs (from your package manager): cmake, ninja, clang-cl + LLVM tools,
# rustup, cargo, curl, tar, git. On a fresh clone the first run does the
# one-time setup (RE-UE4SS clone, xwin binary, Windows SDK splat, rustup
# target) — expect a few minutes + ~640 MB download. Subsequent runs just
# rebuild.
#
# Pass --clean to wipe `build/` before configuring.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

XWIN_VERSION="0.6.6"
XWIN_URL="https://github.com/Jake-Shadle/xwin/releases/download/${XWIN_VERSION}/xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl.tar.gz"

log() { printf '\033[1;36m>>> %s\033[0m\n' "$*"; }
err() { printf '\033[1;31m!!! %s\033[0m\n' "$*" >&2; }

clean=0
for arg in "$@"; do
    case "$arg" in
        --clean) clean=1 ;;
        -h|--help)
            sed -n '2,13p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *) err "unknown arg: $arg"; exit 2 ;;
    esac
done

# --- prereq check -----------------------------------------------------------
missing=()
for cmd in cmake ninja clang-cl rustup cargo curl tar git; do
    command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
done
if (( ${#missing[@]} > 0 )); then
    err "missing tools: ${missing[*]}"
    err "install via your package manager (Arch: 'pacman -S cmake ninja clang rust git')"
    exit 1
fi

# --- step 1: RE-UE4SS clone -------------------------------------------------
if [[ ! -d RE-UE4SS/.git ]]; then
    log "cloning RE-UE4SS (needs access to Re-UE4SS/UEPseudo submodule)"
    git clone --recurse-submodules https://github.com/UE4SS-RE/RE-UE4SS.git
else
    log "RE-UE4SS already present (skipping clone)"
fi

# --- step 2: xwin binary ----------------------------------------------------
XWIN_BIN="${HOME}/.local/bin/xwin"
if [[ ! -x "$XWIN_BIN" ]]; then
    log "installing xwin ${XWIN_VERSION} to ${XWIN_BIN}"
    mkdir -p "$(dirname "$XWIN_BIN")"
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    curl -fsSL "$XWIN_URL" | tar xz -C "$tmp"
    install -m755 "$tmp"/xwin-*/xwin "$XWIN_BIN"
    trap - EXIT
    rm -rf "$tmp"
else
    log "xwin already installed at ${XWIN_BIN}"
fi

# --- step 3: Windows SDK splat ----------------------------------------------
if [[ ! -d xwin/crt || ! -d xwin/sdk ]]; then
    log "splatting Windows SDK + MSVC CRT (~640 MB; one-time)"
    "$XWIN_BIN" --accept-license --arch x86_64 --variant desktop \
        --cache-dir "$PWD/.xwin-cache" splat --output "$PWD/xwin"
else
    log "xwin SDK already splatted (skipping)"
fi

# --- step 4: Rust MSVC target -----------------------------------------------
if ! rustup target list --installed 2>/dev/null | grep -q '^x86_64-pc-windows-msvc$'; then
    log "adding Rust x86_64-pc-windows-msvc target"
    rustup target add x86_64-pc-windows-msvc
else
    log "Rust x86_64-pc-windows-msvc target already installed"
fi

# --- step 5: configure ------------------------------------------------------
if (( clean )); then
    log "--clean: wiping build/"
    rm -rf build
fi
if [[ ! -f build/CMakeCache.txt ]]; then
    log "configuring CMake (Game__Shipping__Win64 via xwin-clang-cl toolchain)"
    XWIN_DIR="$PWD/xwin" cmake -B build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$PWD/RE-UE4SS/cmake/toolchains/xwin-clang-cl-toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Game__Shipping__Win64
else
    log "CMake already configured (skipping; pass --clean to re-configure)"
fi

# --- step 6: build + package ------------------------------------------------
# Target the Package step — it depends on the DLL so this builds both.
log "building"
XWIN_DIR="$PWD/xwin" cmake --build build --target WindroseIdleCpuFixPackage

# --- report -----------------------------------------------------------------
zip="build/WindroseIdleCpuFix.zip"
if [[ -f "$zip" ]]; then
    size="$(du -h "$zip" | cut -f1)"
    log "done: $zip ($size) — extract into <server>/…/ue4ss/Mods/"
else
    err "build succeeded but $zip was not produced"
    exit 1
fi
