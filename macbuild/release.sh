#!/bin/sh
# ---------------------------------------------------------------------------
# Build, sign, notarise and staple NOLF2Launcher.app.
#
#   macbuild/release.sh "Developer ID Application: Your Name (TEAMID)" [profile]
#
# `profile` is a notarytool keychain profile. Create it once with:
#
#   xcrun notarytool store-credentials nolf2-notary \
#       --apple-id you@example.com --team-id TEAMID --password <app-specific-password>
#
# ⚠️ The app-specific password comes from appleid.apple.com, NOT your Apple ID
# password. Store it via the command above so it never appears in a script.
#
# ⚠️ Notarisation UPLOADS the bundle to Apple. This script only does that when
# you pass a profile name; without one it builds and signs locally and stops,
# so you can inspect the result first.
# ---------------------------------------------------------------------------
set -e

IDENTITY=${1:?Usage: release.sh "Developer ID Application: ... (TEAMID)" [notary-profile]}
PROFILE=$2

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build-xcode
APP=$BUILD/Release/NOLF2Launcher.app

# Team ID is the parenthesised suffix of the identity string.
TEAM=$(printf '%s' "$IDENTITY" | sed -n 's/.*(\([A-Z0-9]*\))$/\1/p')
[ -n "$TEAM" ] || { echo "Could not parse a Team ID out of the identity."; exit 1; }

# ⚠️ CLEAR ARCHIVE RESIDUE FIRST.
#
# Product -> Archive in Xcode runs the *install* action and replaces the build
# products with symlinks into DerivedData/.../ArchiveIntermediates/. Once those
# intermediates are cleaned the links dangle, and an ordinary build then fails
# with either "ld: open() failed, errno=2" on its own output or "MkDir ...
# NOLF2Launcher.app" — both of which look like signing faults and are not.
# This script is the supported distribution path, so it must not be derailed by
# whatever happened in the IDE beforehand.
if [ -d "$BUILD" ]; then
    find "$BUILD" -type l 2>/dev/null | while read -r l; do
        case "$(readlink "$l")" in
            *ArchiveIntermediates*) echo "    clearing archive residue: $l"; rm -f "$l" ;;
        esac
    done
fi

echo "==> Configuring"
cmake -S "$ROOT/macbuild" -B "$BUILD" -G Xcode > /dev/null

echo "==> Building Release (signed as $TEAM)"
xcodebuild -project "$BUILD/LithtechMacFoundation.xcodeproj" \
    -target NOLF2Launcher -configuration Release \
    CODE_SIGN_IDENTITY="$IDENTITY" DEVELOPMENT_TEAM="$TEAM" \
    CODE_SIGN_STYLE=Manual OTHER_CODE_SIGN_FLAGS="--timestamp" \
    build > /dev/null

# ⚠️ Sign inner-out. The nested binaries must each carry Developer ID AND the
# hardened runtime, or notarisation is rejected — the enclosing app's signature
# is not enough. Re-signing here is belt-and-braces: the CMake targets already
# set ENABLE_HARDENED_RUNTIME, and this guarantees the timestamp too.
# ⚠️ --identifier EXPLICITLY. These are plain Mach-O files, not bundles, so
# there is no Info.plist for codesign to take an identifier from: without this
# it invents one per build ("Lithtech-55554944aad12..."), which is unstable
# across builds. Xcode's PRODUCT_BUNDLE_IDENTIFIER does not apply to unbundled
# products, so it has to be set here.
echo "==> Signing embedded modules"
sign_one() {   # sign_one <file> <identifier>
    codesign --force --options runtime --timestamp \
             --identifier "$2" --sign "$IDENTITY" "$1"
}
sign_one "$APP/Contents/Frameworks/libCShell.dylib"   com.schonder.nolf2mac.cshell
sign_one "$APP/Contents/Frameworks/libObject.lto"     com.schonder.nolf2mac.object
sign_one "$APP/Contents/Frameworks/libClientFx.dylib" com.schonder.nolf2mac.clientfx
sign_one "$APP/Contents/MacOS/Lithtech"               com.schonder.nolf2mac.lithtech
echo "==> Signing the app"
codesign --force --options runtime --timestamp --sign "$IDENTITY" "$APP"

echo "==> Verifying"
codesign --verify --strict --verbose=2 "$APP"
# Every Mach-O must show the runtime flag; anything at 0x2 would be rejected.
for f in "$APP/Contents/MacOS/"* "$APP/Contents/Frameworks/"*; do
    printf '    %-22s ' "$(basename "$f")"
    codesign -dvv "$f" 2>&1 | grep -o 'flags=0x[0-9a-f]*([a-z,]*)' || echo '(none)'
done

if [ -z "$PROFILE" ]; then
    echo ""
    echo "Built and signed: $APP"
    echo "No notary profile given, so nothing was uploaded."
    echo "Re-run with the profile name to notarise."
    exit 0
fi

ZIP=$BUILD/NOLF2Launcher.zip
echo "==> Submitting to Apple for notarisation"
rm -f "$ZIP"
/usr/bin/ditto -c -k --keepParent "$APP" "$ZIP"
xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait

echo "==> Stapling"
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"

echo ""
echo "Done. Gatekeeper assessment:"
spctl --assess --type execute --verbose=2 "$APP" || true
