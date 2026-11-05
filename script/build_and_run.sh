#!/bin/zsh
# Development build.
#   --build  only builds and signs dist/release/Stillmic.app. Nothing is installed.
#   no flag  also REPLACES /Applications/Stillmic.app and launches it. If you are on a call through
#            Stillmic, your mic goes silent while the app restarts.
# This script never installs the driver or restarts coreaudiod. Use the app's menu for that.
set -euo pipefail

ROOT_DIR="${0:A:h:h}"
export SIGNING_IDENTITY="${STILLMIC_SIGNING_IDENTITY:-Developer ID Application: Robin Obermaier (5S5288W3R7)}"
python3 "$ROOT_DIR/script/package_app.py"

if [[ "${1:-}" == "--build" ]]; then
  echo "$ROOT_DIR/dist/release/Stillmic.app"
  exit 0
fi

/usr/bin/pkill -x Stillmic 2>/dev/null || true
while /usr/bin/pgrep -x Stillmic >/dev/null; do /bin/sleep 0.1; done
/bin/rm -rf /Applications/Stillmic.app
/usr/bin/ditto "$ROOT_DIR/dist/release/Stillmic.app" /Applications/Stillmic.app
/bin/sleep 1  # LaunchServices can reject an open right after the old process exits (error -600)
/usr/bin/open /Applications/Stillmic.app
echo "Installed and launched /Applications/Stillmic.app"
