# macOS bring-up sandbox (Phase 0) -- builds only the portable foundation libs.
# Kept separate from the top-level CMakeLists.txt so the Windows build is untouched.
# Build:  cmake -S macbuild -B build-mac -G Ninja && cmake --build build-mac
cmake_minimum_required(VERSION 3.20)
project(LithtechMacFoundation CXX C)

# The vendored sub-CMakeLists declare cmake_minimum_required(3.5); allow them.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# ⚠️ NO Debug CONFIGURATION ON THE MULTI-CONFIG (Xcode) GENERATOR.
#
# Xcode builds Debug by default, so a fresh ⌘B used to fail immediately — and
# Debug is not a supported configuration for this port:
#
#  * `_DEBUG` pulls in Win32-only debug code (stacktrace.h's <imagehlp.h>, the
#    UberAssert clipboard/_CrtDbgReport tail). Those two are now guarded, but
#  * it still fails to LINK: GetFrameStats, CountPercent::In/Out/Clear and
#    CUIDebug::DebugPrint are Debug-only diagnostics that this port never
#    implemented, and
#  * more fundamentally, the port compiles asserts OUT on purpose for retail
#    parity — the game code is full of diagnostic asserts that were never
#    executed at retail and that abort here.
#
# RelWithDebInfo is the useful middle: full symbols for crash reports, but the
# retail `_FINAL` code path. Re-add "Debug" here if you ever want to take the
# link errors on.
if(CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "" FORCE)
endif()

# ⚠️ DO NOT USE PRODUCT -> ARCHIVE IN XCODE. Use macbuild/release.sh.
#
# Archive runs the *install* action. With CMake's `install_name @rpath/...`,
# Xcode replaced EVERY build product in build-xcode/ with a symlink into
# DerivedData/.../ArchiveIntermediates/.../@rpath/, and once those intermediates
# were cleaned the links dangled — so the next ordinary build died with
#   ld: open() failed, errno=2 ... for '.../ObjectDLL/Release/libObject.lto'
# on its own OUTPUT path. Twenty products were affected; the cure was deleting
# every symlink under build-xcode that pointed into ArchiveIntermediates.
#
# SKIP_INSTALL stops the install action from touching these products at all,
# which keeps an accidental Archive from wrecking the tree again.
set(CMAKE_XCODE_ATTRIBUTE_SKIP_INSTALL "YES")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_C_STANDARD 11)

# This engine targets little-endian 64-bit (arm64/x86_64). No 32-bit on macOS.
# The codebase uses _LINUX/__LINUX as its generic "not-Windows / POSIX" gate, so
# we join that path on macOS and special-case the genuinely Linux-only bits via
# __APPLE__ (predefined by clang). LT_MACOS marks our own divergences.
add_compile_definitions(_LINUX __LINUX LT_MACOS)
add_compile_definitions($<IF:$<CONFIG:Debug>,_DEBUG,_FINAL>)
# Retail parity: NOLF2 shipped with assert() compiled out. The game code is full
# of diagnostic asserts (e.g. LTStrCpy truncation) that abort our build but were
# never executed at retail. Real faults still surface via signals/lldb.
add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

# Force-include the MSVC CRT compat shim into every TU (stricmp, _strupr, ...).
add_compile_options(-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc_crt_compat.h)

# Old (2003-era) code: relax a few clang errors that were warnings back then.
add_compile_options(
	-fdeclspec                  # accept __declspec(dllexport/import/...) on clang
	-fshort-wchar               # 16-bit wchar_t (NOLF/Win assume it; macOS native is 32-bit)
	-fno-strict-aliasing
	-Wno-deprecated-declarations
	-Wno-writable-strings        # string literal -> char*
	-Wno-c++11-narrowing
	-Wno-invalid-offsetof
	-Wno-register
	-Wno-address-of-temporary   # old code passes &LTVector(...) temporaries
	-Wno-return-type
)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
# compat/ first: provides the macOS <windows.h> shim (and friends).
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/compat)
include_directories(${ROOT}/sdk/inc)

# --- Foundation (Phase 0) ---
add_subdirectory(${ROOT}/libs/zlib     ${CMAKE_BINARY_DIR}/zlib)
add_subdirectory(${ROOT}/libs/lith     ${CMAKE_BINARY_DIR}/lith)
add_subdirectory(${ROOT}/libs/stdlith  ${CMAKE_BINARY_DIR}/stdlith)
add_subdirectory(${ROOT}/libs/ltmem    ${CMAKE_BINARY_DIR}/ltmem)
add_subdirectory(${ROOT}/libs/rezmgr   ${CMAKE_BINARY_DIR}/rezmgr)

# --- Next-tier support libs (exercise the windows.h shim) ---
add_subdirectory(${ROOT}/libs/MFCStub  ${CMAKE_BINARY_DIR}/MFCStub)
add_subdirectory(${ROOT}/libs/CryptMgr ${CMAKE_BINARY_DIR}/CryptMgr)
add_subdirectory(${ROOT}/libs/ButeMgr  ${CMAKE_BINARY_DIR}/ButeMgr)
add_subdirectory(${ROOT}/libs/LTGUIMgr ${CMAKE_BINARY_DIR}/LTGUIMgr)
# --- Phase 3 prereq libs (needed by the game DLLs) ---
add_subdirectory(${ROOT}/libs/RegMgr    ${CMAKE_BINARY_DIR}/RegMgr)   # Win32 registry -> winreg.h stub (config uses defaults)
add_subdirectory(${ROOT}/libs/ServerDir ${CMAKE_BINARY_DIR}/ServerDir) # WON/Titan online -> NULL-factory stub (single-player)
# --- Engine support libs ---
add_subdirectory(${ROOT}/runtime/info  ${CMAKE_BINARY_DIR}/info)
add_subdirectory(${ROOT}/runtime/sound ${CMAKE_BINARY_DIR}/sound)
add_subdirectory(${ROOT}/runtime/ui    ${CMAKE_BINARY_DIR}/ui)

# --- Engine server (DLL_Server) -- has a non-WIN32 (linux) sys path ---
add_subdirectory(${ROOT}/runtime/server ${CMAKE_BINARY_DIR}/server)

# --- Engine client (EXE_Lithtech) -- platform shell; portables swapped to linux ---
add_subdirectory(${ROOT}/runtime/client ${CMAKE_BINARY_DIR}/client)

# --- macOS platform shell (Phase 1): Cocoa + OpenGL window + client bridge ---
target_sources(EXE_Lithtech PRIVATE
    ${ROOT}/runtime/kernel/src/sys/macos/macos_window.mm
    ${ROOT}/runtime/kernel/src/sys/macos/macos_input.mm          # NSEvent keyboard/mouse
    ${ROOT}/runtime/kernel/src/sys/macos/macos_client.cpp
    ${ROOT}/runtime/kernel/src/sys/macos/macos_clientshell.cpp   # null IClientShell (Phase 1 bring-up)
    ${ROOT}/runtime/kernel/src/sys/macos/macos_cres.cpp          # cres.dll PE string resources (game UI text)
    ${ROOT}/runtime/kernel/src/sys/macos/macos_music.cpp         # interactive music (.SGT wave-track scheduler)
    ${ROOT}/runtime/render_a/src/sys/nulldib/nullrender.cpp   # renderer front-end (rdll_RenderDLLSetup)
    ${ROOT}/runtime/render_a/src/sys/gl/gl_worlddata.cpp      # Phase-2 GL world loader/draw
    ${ROOT}/runtime/render_a/src/sys/gl/gl_texture.cpp        # Phase-2 GL DTX texture upload
    ${ROOT}/runtime/render_a/src/sys/gl/gl_model.cpp          # Phase-2 GL model meshes + draw
    ${ROOT}/runtime/render_a/src/sys/gl/gl_drawprim.cpp       # Phase-2 GL ILTDrawPrim (2D/console; replaced macos_nulldrawprim)
    ${ROOT}/runtime/render_a/src/sys/gl/gl_renderstyle.cpp    # Phase-2 GL render styles (authored model render state)
    ${ROOT}/runtime/render_a/src/sys/gl/gl_convar.cpp         # renderer console vars (rendererconsolevars.h) -> engine console
    ${ROOT}/runtime/render_a/src/sys/gl/gl_polygrid.cpp       # OT_POLYGRID (water, ice) height-field draw
    ${ROOT}/runtime/render_a/src/sys/gl/gl_particles.cpp      # OT_PARTICLESYSTEM (fire, smoke, waterfall)
    ${ROOT}/runtime/sound/src/sys/macos/s_coreaudio.cpp       # Phase-3 CoreAudio ILTSoundSys (registers the sound factory)
    ${ROOT}/runtime/render_b/src/sys/d3d/d3dtexinterface.cpp) # ILTTexInterface (renderer-agnostic; console font/UI textures)
set_source_files_properties(${ROOT}/runtime/kernel/src/sys/macos/macos_window.mm
    ${ROOT}/runtime/kernel/src/sys/macos/macos_input.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
# -g on the ENGINE as well as the game DLLs. Asserts are compiled out (NDEBUG,
# §5), so crashes arrive as bare signals and the backtrace IS the diagnosis —
# without this, engine frames resolve to a symbol but no source line, which for
# something like CheapLTLink::Remove (a force_inline in a header, called from
# dozens of places) tells you almost nothing. Costs binary size, not speed.
target_compile_options(EXE_Lithtech PRIVATE -g)

target_include_directories(EXE_Lithtech PRIVATE
    ${ROOT}/runtime/kernel/src/sys/macos
    ${ROOT}/runtime/render_a/src/sys/d3d   # d3d_device/texture/renderstatemgr/rendererframestats.h (vs the d3d9 stub)
    ${ROOT}/runtime/render_a/src
    ${ROOT}/runtime/render_b/src           # gendrawprim.h (CGenDrawPrim base for the GL drawprim)
    ${ROOT}/runtime/render_b/src/sys/d3d)  # d3dtexinterface.h
# CoreText/CoreGraphics: CUIVectorFont rasterizes the game's TTF UI fonts
# (runtime/ui/src/cuivectorfont.cpp — the macOS replacement for Win32 GDI).
target_link_libraries(EXE_Lithtech "-framework Cocoa" "-framework OpenGL"
                                   "-framework CoreText" "-framework CoreGraphics"
                                   "-framework AudioToolbox" "-framework AudioUnit"
                                   "-framework CoreAudio")

# --- Game DLLs (Phase 3): the real game logic the engine loads. NOLF2 = "TO2". ---
add_subdirectory(${ROOT}/NOLF2/ObjectDLL/TO2 ${CMAKE_BINARY_DIR}/ObjectDLL)  # -> Object.lto (server game logic)
# The game code leans on MSVC extensions; -fms-extensions helps with the mild
# ones. Do NOT add -fms-compatibility: it stops defining __GNUC__ and un-keywords
# char16_t/char32_t, which poisons the macOS system headers (va_list/unistd/...).
# Forward-declared enums (the big MSVC-ism) are instead fixed in-source by giving
# them a fixed underlying type (enum X : int) — standard C++11.
# -Wno-invalid-token-paste: MSVC's preprocessor pastes then RE-LEXES, so pastes
# that form no single valid token (e.g. `name ## (`) are fine there; with
# -fms-extensions clang accepts them too once the diagnostic is disabled.
target_compile_options(NOLF2_ObjectDLL PRIVATE -fms-extensions -Wno-invalid-token-paste -g)
add_subdirectory(${ROOT}/NOLF2/ClientShellDLL/TO2 ${CMAKE_BINARY_DIR}/ClientShellDLL)  # -> CShell (client game logic)
target_compile_options(NOLF2_ClientShellDLL PRIVATE -fms-extensions -Wno-invalid-token-paste -g)

# --- ClientFX (Phase 4): the effect library. Waterfalls, mist, smoke, spray,
# sparks, muzzle flashes and the menu background art are all ClientFX effects
# (level data references them by name via SpecialFX objects' FxName property).
# Retail ships it as Game/CLIENTFX.FXD, a Win32 DLL, loaded by
# CClientFXDB::LoadFxDll() -- which is wrapped entirely in #ifdef WIN32 and so
# was a silent no-op here, leaving m_nNumEffectTypes = 0 and every effect lookup
# missing. We build it as a normal dylib and dlopen it (see ClientFXDB.cpp).
# NOTE deliberately NOT named ClientFx.fxd: the retail Win32 CLIENTFX.FXD sits in
# the same game directory and the names would collide.
add_subdirectory(${ROOT}/NOLF2/ClientFxDLL ${CMAKE_BINARY_DIR}/ClientFxDLL)
set_target_properties(NOLF2_ClientFXDLL PROPERTIES OUTPUT_NAME ClientFx SUFFIX .dylib)
target_compile_options(NOLF2_ClientFXDLL PRIVATE -fms-extensions -Wno-invalid-token-paste)

# ⚠️ EACH GAME MODULE MUST BE A CLOSED WORLD (Windows DLL semantics).
# libObject.lto and libCShell.dylib are built from a large amount of the SAME
# source (NOLF2/Shared/*: FXButeMgr, WeaponMgr, ModelButeMgr, ... each with its
# own g_p* singleton pointer). On Windows those are two independent DLLs with
# private copies. On macOS, dyld COALESCES weak definitions — C++ template
# instantiations, inline functions and some vtables — across every loaded image
# regardless of two-level namespace: the first image loaded wins for ALL of
# them. libCShell is dlopen'd before libObject, so the server's
# `debug_new(CFXButeMgr)` (= the template `debug_new_fn<CFXButeMgr>`) resolved
# to the CLIENT's instantiation, constructing the server's object with the
# CLIENT's vtable. Its virtual Init() then ran client code, saw the CLIENT's
# already-set g_pFXButeMgr, and bailed — killing every "New Game" mission load.
# Exporting only the engine's dlsym entry points makes everything else private
# to its image, which is exactly the Windows model. Do NOT widen these lists.
target_link_options(NOLF2_ObjectDLL PRIVATE
	"LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports_object.txt")
target_link_options(NOLF2_ClientShellDLL PRIVATE
	"LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports_cshell.txt")
set_property(TARGET NOLF2_ObjectDLL APPEND PROPERTY LINK_DEPENDS
	${CMAKE_CURRENT_SOURCE_DIR}/exports_object.txt)
set_property(TARGET NOLF2_ClientShellDLL APPEND PROPERTY LINK_DEPENDS
	${CMAKE_CURRENT_SOURCE_DIR}/exports_cshell.txt)
target_link_options(NOLF2_ClientFXDLL PRIVATE
	"LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports_clientfx.txt")
set_property(TARGET NOLF2_ClientFXDLL APPEND PROPERTY LINK_DEPENDS
	${CMAKE_CURRENT_SOURCE_DIR}/exports_clientfx.txt)

# ---------------------------------------------------------------------------
# NOLF2Launcher.app — the SwiftUI front end, and the shippable bundle.
#
# ⚠️ GUARDED ON THE XCODE GENERATOR. CMake supports Swift only under Xcode and
# Ninja; enabling it unconditionally would BREAK the "Unix Makefiles" build that
# everything else in this project uses. So:
#
#     cmake -S macbuild -B build-mac    -G "Unix Makefiles"   engine only
#     cmake -S macbuild -B build-xcode  -G Xcode              engine + .app
#
# The Xcode project is what produces a signable, notarizable bundle: it embeds
# the engine and the three dlopen'd game modules, so a tester needs no symlinks
# and no environment set up by hand.
# ---------------------------------------------------------------------------
if(CMAKE_GENERATOR STREQUAL "Xcode")
	enable_language(Swift)

	set(LAUNCHER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../launcher)

	# The background artwork is optional and NOT in the repository (copyrighted
	# promotional material). Bundle it when the user has supplied it locally;
	# the launcher falls back to a plain background when it is absent.
	set(LAUNCHER_RESOURCES "")
	if(EXISTS ${LAUNCHER_DIR}/Resources/NOLF2_Background.jpg)
		list(APPEND LAUNCHER_RESOURCES ${LAUNCHER_DIR}/Resources/NOLF2_Background.jpg)
		message(STATUS "Launcher: bundling background artwork")
	else()
		message(STATUS "Launcher: no background artwork; using the fallback")
	endif()

	# ⚠️ THE ARTWORK IS *NOT* AN XCODE RESOURCE — it is copied in POST_BUILD.
	#
	# Xcode's resource-copy phase preserves extended attributes, and the file
	# carries `com.apple.provenance` (macOS stamps it on downloaded files; it
	# survives `xattr -c` and is re-applied to a fresh copy of the bytes). The
	# copy then fails with a bare
	#   error: copy(...NOLF2_Background.jpg...): Operation not permitted
	# which looks like a build fault and is really a TCC one. Copying it the same
	# way the engine and game modules go in sidesteps the resource phase entirely
	# and does not care about xattrs.
	add_executable(NOLF2Launcher MACOSX_BUNDLE
		${LAUNCHER_DIR}/NOLF2Launcher.swift)

	# ⚠️ CLEAR THE INHERITED C/C++ FLAGS. This directory does
	# `add_compile_options(-include .../msvc_crt_compat.h)` plus a set of clang
	# warning flags, and CMake applies directory options to EVERY target in it —
	# so swiftc receives `-include` and fails with
	# "Driver threw unknown argument: '-include'". Clearing the property on this
	# target alone leaves every engine target's flags exactly as they were;
	# scoping the directory options by COMPILE_LANGUAGE would touch the engine
	# build, which is not worth the risk for one Swift file.
	set_target_properties(NOLF2Launcher PROPERTIES COMPILE_OPTIONS "")

	set_target_properties(NOLF2Launcher PROPERTIES
		OUTPUT_NAME                                     "NOLF2Launcher"
		MACOSX_BUNDLE_BUNDLE_NAME                       "No One Lives Forever 2"
		MACOSX_BUNDLE_GUI_IDENTIFIER                    "com.schonder.nolf2mac.launcher"
		XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER       "com.schonder.nolf2mac.launcher"
		MACOSX_BUNDLE_BUNDLE_VERSION                    "1"
		MACOSX_BUNDLE_SHORT_VERSION_STRING              "0.1.1"
		XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET        "12.0"
		XCODE_ATTRIBUTE_SWIFT_VERSION                   "5.0"
		# Hardened runtime is required for notarisation. Signing everything with
		# ONE Team ID is what lets the engine dlopen the game modules without a
		# library-validation exception — set DEVELOPMENT_TEAM to enable it.
		XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME         "YES"
		XCODE_ATTRIBUTE_CODE_SIGN_STYLE                 "Automatic")

	# The launcher is useless without the engine and the game logic, so building
	# it builds them.
	add_dependencies(NOLF2Launcher
		EXE_Lithtech NOLF2_ObjectDLL NOLF2_ClientShellDLL NOLF2_ClientFXDLL)

	# ★★ HARDENED RUNTIME ON *EVERY* EXECUTABLE, NOT JUST THE APP.
	#
	# Notarisation is a property of the whole submitted bundle: Apple rejects it
	# unless every Mach-O inside is signed with a Developer ID AND carries the
	# hardened-runtime flag. Xcode signs each target with ITS OWN settings and
	# the already-signed binaries keep that signature when they are copied in,
	# so setting the flag only on the launcher is not enough — verified with
	# `codesign -dvv`, which showed the launcher at flags=0x10002(adhoc,runtime)
	# while the engine and all three modules sat at flags=0x2(adhoc), i.e. NO
	# runtime. That bundle would have failed notarisation.
	#
	# ⚠️ Signing everything with ONE Team ID is also what lets the engine
	# dlopen() the three modules under the hardened runtime without needing the
	# com.apple.security.cs.disable-library-validation entitlement.
	foreach(_t EXE_Lithtech NOLF2_ObjectDLL NOLF2_ClientShellDLL NOLF2_ClientFXDLL)
		set_target_properties(${_t} PROPERTIES
			XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME "YES")
	endforeach()

	# ⚠️ SET IDENTIFIERS HERE, NOT IN XCODE'S UI. This project is GENERATED —
	# `cmake -G Xcode` overwrites it, so anything typed into the Xcode
	# inspector is silently lost on the next regeneration. Without an explicit
	# identifier codesign invents one per build (it was
	# "Lithtech-55554944aad12..."), which is unstable across builds.
	set_target_properties(EXE_Lithtech PROPERTIES
		XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.schonder.nolf2mac.lithtech")

	# Assemble the bundle: the engine beside the launcher, the three dlopen'd
	# modules in Frameworks/. The launcher passes absolute paths to them via
	# LT_CSHELL_MODULE / LT_OBJECT_MODULE / LT_CLIENTFX_MODULE, so their exact
	# location only has to be consistent, not special.
	add_custom_command(TARGET NOLF2Launcher POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory
			"$<TARGET_BUNDLE_CONTENT_DIR:NOLF2Launcher>/Frameworks"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:EXE_Lithtech>"
			"$<TARGET_BUNDLE_CONTENT_DIR:NOLF2Launcher>/MacOS/Lithtech"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:NOLF2_ClientShellDLL>"
			"$<TARGET_BUNDLE_CONTENT_DIR:NOLF2Launcher>/Frameworks/libCShell.dylib"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:NOLF2_ObjectDLL>"
			"$<TARGET_BUNDLE_CONTENT_DIR:NOLF2Launcher>/Frameworks/libObject.lto"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:NOLF2_ClientFXDLL>"
			"$<TARGET_BUNDLE_CONTENT_DIR:NOLF2Launcher>/Frameworks/libClientFx.dylib"
		COMMENT "Embedding engine and game modules into NOLF2Launcher.app")

	# The optional background artwork, copied the same way for the reason above.
	if(LAUNCHER_RESOURCES)
		add_custom_command(TARGET NOLF2Launcher POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory
				"$<TARGET_BUNDLE_CONTENT_DIR:NOLF2Launcher>/Resources"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${LAUNCHER_DIR}/Resources/NOLF2_Background.jpg"
				"$<TARGET_BUNDLE_CONTENT_DIR:NOLF2Launcher>/Resources/NOLF2_Background.jpg"
			COMMENT "Adding launcher background artwork")
	endif()
endif()
