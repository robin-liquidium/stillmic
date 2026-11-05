#!/bin/bash
# Builds and installs NoNoise to /Applications. The driver is separate: see install-driver.sh.
set -euo pipefail
cd "$(dirname "$0")"

swift build -c release --arch arm64
./bundle.sh
pkill -x NoNoiseMac 2>/dev/null || true
while pgrep -x NoNoiseMac >/dev/null; do sleep 0.1; done
rm -rf /Applications/NoNoiseMac.app   # needs sudo if a root-owned upstream .pkg install is there
ditto NoNoiseMac.app /Applications/NoNoiseMac.app
sleep 1   # LaunchServices can reject an open right after the old process exits (error -600)
open /Applications/NoNoiseMac.app
echo "Installed /Applications/NoNoiseMac.app"
