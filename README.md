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

* Apple Silicon Mac (arm64), macOS 15 or later
* Xcode 16 or later (to build)
* A retail NOLF2 installation

## Building

The build is the Xcode project at the root of the repository:

```sh
open NOLF2.xcodeproj
```

Choose the **NOLF2Launcher** scheme and build or run it. That builds the engine,
the three game modules and the launcher, and assembles them into one bundle:

```
NOLF2Launcher.app/Contents/MacOS/NOLF2Launcher    the launcher
NOLF2Launcher.app/Contents/MacOS/Lithtech         the engine
NOLF2Launcher.app/Contents/Frameworks/            libCShell / libObject / libClientFx
```

From the command line:

```sh
xcodebuild -project NOLF2.xcodeproj -scheme NOLF2Launcher -configuration Release build
```

> ⚠️ **Signing:** set your own team under *Signing & Capabilities* (a free
> personal team is enough). "Sign to Run Locally" builds, but macOS then refuses
> to load the game modules into the engine and the game shows a black screen.
> `macbuild/BUILD_NOTES.md` explains why, along with the reasons behind the
> other build settings.

### Distributing

Product › Archive (scheme **NOLF2Launcher**), then *Distribute App › Direct
Distribution* in the Organizer: it signs with your Developer ID, notarises and
staples. `macbuild/release.sh` does the same from the command line.

## The launcher

The launcher asks for your retail folder, validates it, lets you add extra
`.rez` archives (loaded in order — later entries override earlier ones), and
starts the engine with the correct working directory and the absolute paths of
the three game modules.

If the game ever fails to start, the engine's output is captured to
`~/Library/Logs/NOLF2Mac/engine.log` — please attach it to a bug report.

Its settings live in `~/Library/Application Support/NOLF2Mac/`. **Nothing is
ever written into your game folder.**

## Running directly

Without the launcher, give the engine the three modules yourself:

```sh
APP=/path/to/NOLF2Launcher.app
cd "/path/to/No One Lives Forever 2"
LT_LOAD_CSHELL=1 \
LT_CSHELL_MODULE="$APP/Contents/Frameworks/libCShell.dylib" \
LT_OBJECT_MODULE="$APP/Contents/Frameworks/libObject.lto" \
LT_CLIENTFX_MODULE="$APP/Contents/Frameworks/libClientFx.dylib" \
"$APP/Contents/MacOS/Lithtech" -rez GAME.rez -rez GAME2.rez -rez Update_v1x3.rez -rez Game
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
