#!/bin/bash
# Assembles and ad-hoc signs NoNoiseMac.app from a release build.
set -euo pipefail
cd "$(dirname "$0")"

APP="NoNoiseMac.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/NoNoiseMac "$APP/Contents/MacOS/"
strip -x "$APP/Contents/MacOS/NoNoiseMac"
cp Resources/Info.plist "$APP/Contents/"
cp Resources/dfn3.weights Resources/AppIcon.icns "$APP/Contents/Resources/"
codesign --force --sign - --entitlements Resources/NoNoiseMac.entitlements "$APP"
codesign --verify --strict "$APP"
echo "Bundled $APP"
