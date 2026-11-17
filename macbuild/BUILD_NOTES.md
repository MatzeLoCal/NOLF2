# Building the macOS port — why the project is set up the way it is

`NOLF2.xcodeproj` (repository root) is the whole build: 20 native targets, no
scripts that call other build tools, no generated project. These notes keep the
reasons behind its settings, which used to live as comments in the CMake build
it replaced. Read the relevant section before changing a setting it mentions.

## Targets and products

| Target | Product | Notes |
|---|---|---|
| `NOLF2Launcher` | `NOLF2Launcher.app` | SwiftUI launcher; embeds everything below |
| `EXE_Lithtech` | `Lithtech` | the engine, copied to `Contents/MacOS/` |
| `NOLF2_ClientShellDLL` | `libCShell.dylib` | client game logic, `Contents/Frameworks/` |
| `NOLF2_ObjectDLL` | `libObject.lto` | server game logic, `Contents/Frameworks/` |
| `NOLF2_ClientFXDLL` | `libClientFx.dylib` | effects library, `Contents/Frameworks/` |
| `DLL_Server` | `libServer.dylib` | built, not embedded in the app (as in the CMake build) |
| 14 static libraries | `lib*.a` | linked into the targets above |

The launcher finds the engine beside itself and passes the three module paths
to it as `LT_CSHELL_MODULE`, `LT_OBJECT_MODULE`, `LT_CLIENTFX_MODULE`.

⚠️ **The module file names are load-bearing.** `libClientFx.dylib` is
deliberately not `ClientFx.fxd`: the retail Win32 `CLIENTFX.FXD` sits in the
same game folder and the names would collide. A dynamic-library target needs
`EXECUTABLE_PREFIX = lib` set explicitly, or Xcode builds `CShell.dylib`.

**Adding a source file:** add it to the target(s) that compile it, and only
those. Folders here hold Windows-only sources next to portable ones, so the
project lists files explicitly instead of compiling whole folders. Headers are
in the project for navigation only.

## Configurations

`Release` and `RelWithDebInfo` only — **there is no Debug.** `_DEBUG` pulls in
Win32-only diagnostics, several Debug-only functions (`GetFrameStats`,
`CountPercent`, `CUIDebug::DebugPrint`) were never implemented on this port, and
the port compiles asserts out on purpose for retail parity: the game code is
full of diagnostic asserts that never ran at retail and abort here.
`RelWithDebInfo` is the configuration for running from Xcode: full symbols, the
retail `_FINAL` code path.

## Compiler settings (project level)

* **`-O2` everywhere.** The port once ran at `-O0` without anyone noticing, and
  a profile showed one-line accessors at the top of the stack. `-O2` rather
  than `-O3`: the extra vectorisation buys little and enlarges the binaries.
* **`-fno-strict-aliasing`** is what makes `-O2` safe on 2003-era code.
* **Prefix header `macbuild/compat/msvc_crt_compat.h`** (not precompiled) is
  force-included into every C/C++/Objective-C file: the MSVC CRT helpers the
  engine uses (`stricmp`, `_strupr`, …). It must not reach Swift, and Xcode's
  prefix header does not.
* **`-fshort-wchar`**: the code assumes a 16-bit `wchar_t`, as on Windows.
* **`-fdeclspec`** and the `-Wno-…` list: 2003-era code that clang rejects or
  warns about by default (`__declspec`, string literals to `char*`, narrowing,
  `&` of temporaries, missing returns).
* **Defines** `_LINUX __LINUX LT_MACOS _FINAL NDEBUG` everywhere. The codebase
  uses `_LINUX` as its generic "not Windows" switch; `LT_MACOS` marks this
  port's own changes.
* **`-g`** (`GCC_GENERATE_DEBUGGING_SYMBOLS`) on the engine and the two big game
  modules even in Release: asserts are compiled out, so crashes arrive as bare
  signals and the backtrace is the diagnosis.

## The three game modules

* **`-fms-extensions -Wno-invalid-token-paste`**, for MSVC-isms in the game
  code. ⚠️ Never `-fms-compatibility`: it stops defining `__GNUC__` and breaks
  the macOS system headers. Forward-declared enums are fixed in the source
  (`enum X : int`) instead.
* ⚠️ **Each module is a closed world — `EXPORTED_SYMBOLS_FILE` =
  `macbuild/exports_*.txt`. Do not widen those lists.** `libObject.lto` and
  `libCShell.dylib` compile much of the same source (`NOLF2/Shared/*`, each with
  its own `g_p…` singleton). On Windows they are two DLLs with private copies;
  on macOS dyld coalesces weak definitions (template instantiations, inline
  functions, some vtables) across every loaded image, first image wins.
  `libCShell` loads first, so the server's `debug_new(CFXButeMgr)` built its
  object with the client's vtable and every New Game load failed. Exporting
  only the functions the engine `dlsym()`s restores the Windows model.

## Objective-C: ARC on five files only

The Metal renderer (`mtl_device.mm`, `mtl_texture.mm`, `mtl_drawprim.mm`,
`mtl_world.mm`, `mtl_model.mm`) is compiled with `-fobjc-arc` as a per-file
flag. Without it the render command encoder is released at the end of
`BeginFrame` and Metal aborts with "Command encoder released without
endEncoding". `macos_window.mm` and `macos_input.mm` are deliberately manual
retain/release and have been for the whole port; switching them is a
behaviour change to be made as its own step, if ever.

## Signing and distribution

* **Hardened runtime on every executable, one Team ID for all of them.**
  Notarisation rejects a bundle unless every Mach-O inside is signed with a
  Developer ID and carries the hardened-runtime flag. The same Team ID is what
  lets the engine `dlopen()` the modules under library validation without the
  `disable-library-validation` entitlement.
* ⚠️ **Therefore "Sign to Run Locally" (ad-hoc) does not work.** The engine
  starts, but macOS rejects the modules ("Library Validation failed … Team ID:
  none") and the game stays on a black screen. Building with any team — a free
  personal team is enough — works. Contributors: set your own team in
  *Signing & Capabilities*; do not commit it.
* **Signing identifiers** (`com.schonder.nolf2mac.*`) are passed with
  `--identifier` in `OTHER_CODE_SIGN_FLAGS`. Xcode derives the identifier from
  the bundle identifier only for bundles; a bare dylib or tool would otherwise
  be signed as `libCShell`, `Lithtech`, … The embedded copies keep it.
* **Deployment target 15.0 for every target** (one project-level setting). The
  CMake build set it only for the launcher, so the engine and modules silently
  required the macOS version of whatever SDK built them (27.2 for 0.5).
* **Launcher artwork** (`launcher/Resources/`, copyrighted, not in the
  repository) is copied by a script phase with `cp -X`, not as a bundle
  resource: a listed-but-missing resource breaks a clean clone, and Xcode's
  resource copy keeps the `com.apple.provenance` attribute these files carry,
  which fails as "Operation not permitted".
* **Version:** the project build settings `MARKETING_VERSION` (short version)
  and `CURRENT_PROJECT_VERSION` (build). `launcher/Info.plist` reads them, and
  the launcher reads the bundle, so this is the one place to change it.

**To distribute:** Product › Archive with the `NOLF2Launcher` scheme, then
Distribute App › Direct Distribution in the Organizer (signs with Developer ID,
notarises, staples). `macbuild/release.sh` does the same from the command line.
