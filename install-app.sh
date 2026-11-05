#!/bin/bash
# Builds and installs NoNoise to /Applications. The driver is separate: see install-driver.sh.
set -euo pipefail
cd "$(dirname "$0")"

swift build -c release --arch arm64
./bundle.sh
pkill -x NoNoiseMac 2>/dev/null || true
rm -rf /Applications/NoNoiseMac.app   # needs sudo if a root-owned upstream .pkg install is there
ditto NoNoiseMac.app /Applications/NoNoiseMac.app
open /Applications/NoNoiseMac.app
echo "Installed /Applications/NoNoiseMac.app"
