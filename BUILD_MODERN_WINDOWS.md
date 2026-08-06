# NOLF2 — modern Windows port (starting point)

This folder is a **self-contained build tree** for getting *No One Lives Forever 2*
running from source on a modern Windows machine (Windows 10/11 + Visual Studio 2022).

It is a trimmed copy of the GPL release of the **Jupiter EX engine**
(github.com/jsj2008/lithtech): engine runtime + NOLF2 game code + all the libraries
they depend on, and nothing else.

## Why NOLF2 and not NOLF1

NOLF2 was the natural choice — it is the only game this engine actually builds and runs.
NOLF1 runs on the older **LithTech 2.x** engine, whose source was never released (only
SDK headers + a closed binary), and NOLF1's game code has not been ported onto the
Jupiter engine (`BUILD_NOLF=OFF` upstream, does not build/run). See `../NOLF/README.md`.

## What's included / excluded

Included: `CMakeLists.txt`, `runtime/` (engine), `sdk/`, `libs/` (deps), `NOLF2/` (game).
Excluded (not needed for a NOLF2 build): the other games (Shogo, FEAR, Blood2, NOLF1,
Tron), the MFC-based `tools/`, and the git history.

> Note: the NOLF2 folder also contains the Tron 2.0 game code under `NOLF2/*/TRON`
> (Tron shared NOLF2's source layout). It is gated off by default (`BUILD_TRON=OFF`).

## Prerequisites (must be installed separately)

1. **Visual Studio 2022** — workload "Desktop development with C++".
   Add the **MFC** component if you later build tools.
2. **CMake 3.5+** (3.20+ recommended). The top-level minimum was bumped from 2.8 to 3.5
   so it configures on current CMake; sub-projects still emit harmless deprecation warnings.
3. **DirectX SDK (June 2010)** — **required**. The renderer is Direct3D 9 and pulls in
   `d3dx9` (63 files), the sound driver uses DirectSound 8 (`s_dx8`), and DirectShow base
   classes are used for video. `d3dx9`/`dsound8` are *not* in the modern Windows SDK; only
   the June 2010 DX SDK provides them.
   - Known installer gotcha: error **S1023**. Fix = uninstall the
     "Microsoft Visual C++ 2010 x86/x64 Redistributable" packages, install the DX SDK,
     then reinstall the redists.
4. **Retail NOLF2 game assets** (`.rez`/`.dat`) — not included here (copyright). Needed to
   actually launch; the compiled DLLs/EXE replace/augment a retail install.

## Build — recommended path: 32-bit (Win32) first

Quick start (does everything below, with prerequisite checks):

```bat
build.bat            REM Release, Win32  (recommended)
build.bat Debug      REM Debug, Win32
build.bat Release x64
build.bat clean
```

Or drive CMake yourself:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

The DirectX SDK include/lib paths are resolved from the `DXSDK_DIR` environment
variable (set by the SDK installer) — they are no longer hardcoded, so it works
regardless of which DX SDK you install or where. `build.bat` checks `DXSDK_DIR`
is set before configuring.

**Build Win32 (x86), not x64, for the fast path.** Windows 11 runs 32-bit apps natively,
and x86 sidesteps this engine's deep 32-bit-pointer assumptions (see Known issues). x86
Release is the configuration that actually worked upstream.

## Known issues / the real work ahead

- **Debug build crashes in the C runtime.** Upstream notes secure-CRT / STL iterator
  exceptions in Debug; only a couple were fixed. **Build Release first.** VS2022's iterator
  debug checks are stricter than VS2010's, so expect a few more to fix.
- **x64 is not reliable yet.** The engine is 32-bit-pointer-bound:
  - `libs/ltmem/generalheap.h` is a custom allocator that tags free-list flags into the
    *high bits* of pointers and masks them with `GENERALHEAPFLAGMASK 0x0fffffff` (28 bits) —
    it physically cannot hold a 64-bit pointer. For x64 it must be redesigned or bypassed
    (delegate to system `malloc`/`free`).
  - There are ~215 `(uint32)pointer` truncation sites engine-wide to convert to `uintptr_t`.
  This is why Win32 is the recommended first target.
- **D3DX9 is deprecated.** Fine for getting it running (with the June 2010 SDK). Long-term,
  replace D3DX with DirectXMath + `D3DCompile` to drop the legacy SDK dependency. (~70% of
  D3DX use here is just matrix/vector math — an easy swap.)

## Provenance & license

Engine + game **source**: GPL (the Jupiter EX GPL release). You may modify and redistribute
the source under the GPL. **Game assets remain separate copyright** and are not included.

## A few portability fixes already applied

Carried over from earlier cross-platform work (all benign on Windows):
`libs/lith/baselistcounter.h` (removed an invalid explicit base-constructor call),
`libs/stdlith/memory.h` (`malloc.h` guarded for Apple only), and
`libs/stdlith/l_allocator.cpp` (alignment casts use `uintptr_t`).
