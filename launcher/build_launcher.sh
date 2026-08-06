#!/bin/sh
# ---------------------------------------------------------------------------
# Build NOLF2Launcher.app.
#
#   ./launcher/build_launcher.sh [output-dir]     (default: build-mac)
#
# Produces a self-contained .app. The engine is NOT copied in here — for a
# shipping bundle, `Lithtech` and the three dlopen'd game modules go into
# Contents/MacOS and Contents/Frameworks and are code-signed inner-out with a
# single Team ID (hardened runtime refuses to dlopen unsigned modules).
# During development the launcher just asks you to locate the engine once and
# remembers it.
# ---------------------------------------------------------------------------
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$(cd "$HERE/.." && pwd)/build-mac}
APP="$OUT/NOLF2Launcher.app"

echo "Building $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>NOLF2 Launcher</string>
    <key>CFBundleDisplayName</key>       <string>No One Lives Forever 2</string>
    <key>CFBundleIdentifier</key>        <string>org.nolf2mac.launcher</string>
    <key>CFBundleExecutable</key>        <string>NOLF2Launcher</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>CFBundleVersion</key>           <string>1</string>
    <key>LSMinimumSystemVersion</key>    <string>12.0</string>
    <key>NSHighResolutionCapable</key>   <true/>
    <key>NSPrincipalClass</key>          <string>NSApplication</string>
</dict>
</plist>
PLIST

# ---------------------------------------------------------------------------
# Background artwork, if the user has supplied it.
#
# ⚠️ NOT IN THE REPOSITORY. launcher/Resources/ is git-ignored: the artwork is
# copyrighted promotional material (it carries its own trademark notice), and
# this project does not redistribute assets it has no right to — the same rule
# that keeps the retail game data out. The launcher falls back to a plain
# treatment when it is absent, so a clean clone still builds and runs.
# ---------------------------------------------------------------------------
if [ -f "$HERE/Resources/NOLF2_Background.jpg" ]; then
    cp "$HERE/Resources/NOLF2_Background.jpg" "$APP/Contents/Resources/"
    echo "  + background artwork"
else
    echo "  - no background artwork (launcher/Resources/NOLF2_Background.jpg); using fallback"
fi

swiftc \
    -O \
    -target arm64-apple-macos12.0 \
    -parse-as-library \
    -framework SwiftUI -framework AppKit \
    -o "$APP/Contents/MacOS/NOLF2Launcher" \
    "$HERE/NOLF2Launcher.swift"

# Ad-hoc signature so it runs locally. A release build re-signs with the real
# Developer ID, hardened runtime, then notarises and staples.
codesign --force --sign - "$APP" 2>/dev/null || true

echo "Built: $APP"
echo "Run:   open \"$APP\""
