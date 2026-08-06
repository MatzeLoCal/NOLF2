# No One Lives Forever 2 — native macOS (Apple Silicon) source port

A native **arm64 macOS** source port of the Jupiter EX engine, running
*No One Lives Forever 2: A Spy in H.A.R.M.'s Way*.

The retail campaign is playable: OpenGL renderer, keyboard/mouse input,
CoreAudio sound with streaming MP3 dialogue and music, ClientFX, fog, animated
water, reflective world surfaces, and per-object model lighting.

> **This repository contains source code only.**
> It ships **no game assets**. You need your own retail installation of
> No One Lives Forever 2 to play — the launcher asks you to point at it.

---

## Status

Playable and played through several chapters. Expect beta-quality rough edges;
see [Known issues](#known-issues).

## Requirements

* Apple Silicon Mac (arm64), macOS 12 or later
* Xcode command line tools, and CMake 3.20+
* A retail NOLF2 installation

## Building

```sh
cmake -S macbuild -B build-mac -G "Unix Makefiles"
cmake --build build-mac --target EXE_Lithtech -- -j8
cmake --build build-mac --target DLL_Server NOLF2_ObjectDLL NOLF2_ClientShellDLL NOLF2_ClientFXDLL -- -j8
```

### Xcode

The Xcode project is **generated**, not checked in — that keeps it from drifting
out of sync with the CMake source lists:

```sh
cmake -S macbuild -B build-xcode -G Xcode
open build-xcode/LithtechMacFoundation.xcodeproj
```

## The launcher (and the app bundle)

The launcher, the engine and the three game modules are assembled into a single
`NOLF2Launcher.app` by the **Xcode** build:

```sh
cmake -S macbuild -B build-xcode -G Xcode
open build-xcode/LithtechMacFoundation.xcodeproj
```

Build the `NOLF2Launcher` scheme (or `xcodebuild -target NOLF2Launcher`). The
resulting bundle contains everything:

```
NOLF2Launcher.app/Contents/MacOS/NOLF2Launcher    the launcher
NOLF2Launcher.app/Contents/MacOS/Lithtech         the engine
NOLF2Launcher.app/Contents/Frameworks/            libCShell / libObject / libClientFx
```

> The Swift target is defined only for the Xcode generator — CMake supports
> Swift under Xcode and Ninja, not Unix Makefiles. The Makefiles build below
> still builds the engine exactly as before.

The launcher asks for your retail folder, validates it, lets you add extra
`.rez` archives (loaded in order — later entries override earlier ones), and
starts the engine with the correct working directory and the absolute paths of
the three game modules.

If the game ever fails to start, the engine's output is captured to
`~/Library/Logs/NOLF2Mac/engine.log` — please attach it to a bug report.

Its settings live in `~/Library/Application Support/NOLF2Mac/`. **Nothing is
ever written into your game folder.**

## Running directly

Point the build at your retail installation:

```sh
cd "/path/to/No One Lives Forever 2"
/path/to/build-mac/client/Lithtech -rez GAME.rez -rez GAME2.rez -rez Update_v1x3.rez -rez Game
```

To boot straight into a level:

```sh
... -rez Game +runworld "Worlds/RetailSinglePlayer/c01s01"
```

## Known issues

* **A distant-waterfall ambient sound can produce a loud noise in c01s01.**
  Workaround: set `LT_MUTE_SOUNDS="waterfall_lg_dist"` in the environment. The
  near waterfall still plays. Under investigation.
* Pressing Escape on the Display Options screen can crash: the renderer
  enumerates no display modes, so the list is empty.
* An intermittent crash during texture unloading (`r_UnloadSystemTexture`).
* 3D positional audio is newly enabled; if audio misbehaves, `LT_NO_SOUND_3D=1`
  restores the previous 2D-only mixing and that result is itself useful
  diagnostic information.

## Reporting a bug

Please include:

1. What you were doing, and **where** — the `whereismymonkey` cheat prints the
   level name, and the coordinates shown in the HUD help enormously.
2. The crash report, if it crashed:
   `~/Library/Logs/DiagnosticReports/Lithtech-*.ips`
3. Whether the problem persists after a clean relaunch.

A precise, narrow report is worth far more than a general one. "It starts when I
step past this rock and stops if I disable X" is the kind of detail that finds a
bug quickly.

## Provenance and licensing

This port derives from the public release of the Jupiter EX engine source
(PC Enterprise Edition Build 69), which included the NOLF2 game code.

> ⚠️ **The licensing position of the original release needs to be stated
> accurately here before this repository is made public.** The upstream release
> is widely described as a GPL release, but this source tree carries no license
> file, and individual source headers still bear
> `(c) 1998-1999 Monolith Productions, Inc. All Rights Reserved`.
> Resolve this — ideally by carrying over the upstream project's own license
> text — and replace this notice with the actual terms.

The macOS port work in this repository is offered under the same terms as the
upstream release, whatever those are determined to be.

No One Lives Forever 2 and its assets remain the property of their respective
rights holders. **No game assets are distributed here.**
