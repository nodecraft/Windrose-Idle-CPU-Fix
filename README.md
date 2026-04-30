# WindroseIdleCpuFix

# Notice
This is no longer needed as of 04/30/2026. Windrose has patched the server/client to fix this entirely!! This repo has been archived. 
---

A [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) C++ mod that applies, at
process startup, the same busy-spin throttle that
[shipstuff/windrose-self-hosted's `patch-idle-cpu.py`](https://github.com/shipstuff/windrose-self-hosted/blob/main/scripts/patch-idle-cpu.py)
applies to the exe on disk. Useful if you'd rather not modify the shipped
binary — the mod patches the live-mapped image each launch and leaves the
file untouched.

> **Heads-up:** this project was primarily AI-authored. The C++, CMake,
> README, and Linux cross-compile recipe were all written by Claude
> (Anthropic) in an interactive session, with a human operator directing the
> work, running the builds, and testing in a real UE4SS server process. Read
> the code before trusting it in production — the signature scan and live
> `VirtualProtect`-patched trampoline do modify a running proprietary binary.

## What it does

`WindroseServer-Win64-Shipping.exe` runs Boost.Asio, whose
`socket_select_interrupter::reset()` drain loop busy-spins under Wine/Proton
and burns ~2 CPU cores on an otherwise idle server. The Python script inserts
a 38-byte trampoline at a CC-padding cave in `.text` that calls
`KERNEL32!Sleep(1)` on the loop-continue tail; the loop still exits via its
fast branch when real packets arrive, so the detour is cold under load.

This mod does the same thing at runtime:

1. Scans `.text` for the 9-byte signature `48 8B 0B 8B C7 87 41 34 E9` (must
   be unique; refuses otherwise).
2. Parses the PE's exception directory for `.pdata` ranges and finds the
   largest CC-padding cave ≥38 bytes in `.text` that doesn't overlap any
   `RUNTIME_FUNCTION`.
3. Resolves the `kernel32!Sleep` IAT slot by walking the import directory.
4. Emits the trampoline and redirects the patch site with a `rel32 JMP`,
   under `VirtualProtect` + `FlushInstructionCache`.
5. If the site already jumps into our prologue (re-entry or prior invocation
   in the same process), no-ops.

## Prerequisites

The output is an x64 Windows DLL. Two supported host paths:

**Windows host** (official UE4SS path):
- Visual Studio 2022 with "Desktop development with C++" workload
- CMake ≥ 3.22
- Git with submodule support
- Optional: Ninja (faster incremental builds)

**Linux host, cross-compile** (using UE4SS's shipped `xwin-clang-cl` toolchain):
- `clang-cl` + LLVM tools (`clang`, `lld-link`, `llvm-lib`, `llvm-rc`)
- Rust + `rustup target add x86_64-pc-windows-msvc` (UE4SS's proxy generator is Rust)
- `xwin` binary (download prebuilt from [Jake-Shadle/xwin releases](https://github.com/Jake-Shadle/xwin/releases))
- CMake ≥ 3.22, Ninja, Git

## Layout

This repo follows the layout in
[the UE4SS C++ mod guide](https://docs.ue4ss.com/dev/guides/creating-a-c++-mod.html):

```
windrose-patch-idle/
├── CMakeLists.txt              # top-level: adds RE-UE4SS + the mod
├── RE-UE4SS/                   # you clone this (see Build step 1)
├── WindroseIdleCpuFix/
│   ├── CMakeLists.txt
│   ├── dllmain.cpp             # CppUserModBase subclass, start_mod/uninstall_mod
│   ├── Patcher.hpp
│   └── Patcher.cpp             # signature scan + trampoline emit
├── .gitignore
└── README.md
```

## Build

All commands from the repo root.

**1. Clone UE4SS as a sibling of the mod dir, with its submodules:**

```bash
git clone --recurse-submodules https://github.com/UE4SS-RE/RE-UE4SS.git
```

(If you already cloned without `--recurse-submodules`:
`git -C RE-UE4SS submodule update --init --recursive`.)

The top-level `CMakeLists.txt` expects this exact `RE-UE4SS/` directory name.

**2a. Build on Windows with MSVC.**

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64
cmake --build build --target WindroseIdleCpuFix
```

Or with the Visual Studio generator:
```powershell
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --target WindroseIdleCpuFix --config Game__Shipping__Win64
```

**2b. Build on Linux with clang-cl + xwin.**

The repo ships `./build.sh` — one idempotent script that does the clone of
RE-UE4SS, the xwin binary download, the Windows SDK splat, the Rust target
install, the CMake configure, and the build. Re-run it any time; steps that
are already done get skipped.

```bash
./build.sh           # first run: ~640 MB SDK download + full build
./build.sh           # subsequent runs: just rebuild if sources changed
./build.sh --clean   # wipe build/ and reconfigure
```

Manual equivalent if you'd rather not use the script:

```bash
curl -sL https://github.com/Jake-Shadle/xwin/releases/download/0.6.6/xwin-0.6.6-x86_64-unknown-linux-musl.tar.gz \
  | tar xz -C /tmp && install -m755 /tmp/xwin-0.6.6-x86_64-unknown-linux-musl/xwin ~/.local/bin/xwin

# Keep cache on the same filesystem as the output — xwin uses rename(), not copy.
~/.local/bin/xwin --accept-license --arch x86_64 --variant desktop \
  --cache-dir "$PWD/.xwin-cache" splat --output "$PWD/xwin"

rustup target add x86_64-pc-windows-msvc

XWIN_DIR="$PWD/xwin" cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=RE-UE4SS/cmake/toolchains/xwin-clang-cl-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Game__Shipping__Win64

XWIN_DIR="$PWD/xwin" cmake --build build --target WindroseIdleCpuFixPackage
```

Pick the UE4SS configuration that matches the target game build. The Windrose
retail server is `Game__Shipping__Win64`; see `UE4SS.log`'s
`UE4SS Build Configuration:` line to confirm which one the server-side UE4SS
was built as. The mod must match.

Build outputs (at the build-root):
- `build/WindroseIdleCpuFix/WindroseIdleCpuFix.dll` — the raw DLL.
- `build/WindroseIdleCpuFix.zip` — drop-in install archive, staged by a
  POST_BUILD step on every successful build.

## Install

The easy path: extract `build/WindroseIdleCpuFix.zip` straight into the
server's `ue4ss/Mods/` directory. It produces:

```
<server-install>/R5/Binaries/Win64/ue4ss/Mods/WindroseIdleCpuFix/
    enabled.txt
    dlls/main.dll
```

Manual path (if you prefer copying the DLL by hand): same layout — create
`WindroseIdleCpuFix/dlls/main.dll` (renamed from `WindroseIdleCpuFix.dll`)
and an empty `enabled.txt` next to it.

Some UE4SS versions also want the mod listed in `ue4ss/Mods/mods.txt`:

```
WindroseIdleCpuFix : 1
```

Start the server and look for one of these lines in `UE4SS.log`:

- `[WindroseIdleCpuFix] patched: site=0x... trampoline=0x... loop_top=0x... sleep_iat=0x...`
- `[WindroseIdleCpuFix] already patched in this process`
- `[WindroseIdleCpuFix] patch failed: <reason>`

If `patch failed: signature not found` or `…matched multiple sites`, the
target function has drifted enough that the byte pattern no longer uniquely
identifies the patch site. The upstream Python patcher has the same problem
on that build and would need a new signature from reversing the current
binary.

## Reverting

Stop the server and remove the mod directory (or its `enabled.txt`). The
on-disk binary was never modified, so there's nothing to undo.

## Caveats

- UE4SS is a Windows-client-oriented modding framework. It injects via
  proxy-DLL hijacking (default `dwmapi.dll`, configurable). Headless
  dedicated-server builds under Proton/Wine aren't a well-trodden UE4SS
  deployment — verify UE4SS actually loads into your server process first
  (`UE4SS.log` appearing next to the exe is the signal).
- The Python on-disk patcher is still simpler if you just want the fix once
  per install. This mod's advantage is self-adapting to new builds without
  re-running a script, and leaving the shipped binary untouched.
- Like the Python patcher, this modifies a proprietary binary (in memory,
  this time). No warranty; see the upstream script's disclaimer — same terms.

## Attribution

- The patch itself — the 9-byte signature, 38-byte trampoline layout,
  `.pdata`-aware CC-cave selection, RDX-preserving register saves, and
  IAT-indirect `Sleep(1)` call — is a direct port of
  [shipstuff/windrose-self-hosted's `patch-idle-cpu.py`](https://github.com/shipstuff/windrose-self-hosted/blob/main/scripts/patch-idle-cpu.py).
  All of the reverse engineering and the decision about *what* to patch
  belongs to that project; this repo only re-homes the same bytes into a
  UE4SS-loaded DLL.
- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) (RE-UE4SS) provides the
  DLL-injection loader, the `CppUserModBase` scaffolding this mod plugs into,
  and the `xwin-clang-cl` toolchain file this repo's Linux build path uses.
- The Linux cross-compile recipe relies on
  [Jake-Shadle/xwin](https://github.com/Jake-Shadle/xwin) to fetch Microsoft's
  redistributable CRT + Windows SDK.
- Code, build config, and documentation in this repo were primarily written
  by Claude (Anthropic) via Claude Code, working from the upstream Python
  patcher as reference and from UE4SS's shipped headers and docs. A human
  operator directed the work, made the architectural calls, ran every build,
  and validated behavior against a live UE4SS instance.
