"""Shared Sparkle configuration and nested-code packaging for both build paths."""

import json
import plistlib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONFIG = json.loads((ROOT / "sparkle.json").read_text())
PUBLIC_KEY = CONFIG["public_key"]
ARTIFACT = ROOT / ".build/artifacts/sparkle/Sparkle"


def tool(name: str) -> Path:
    path = ARTIFACT / "bin" / name
    if not path.is_file():
        subprocess.run(["swift", "package", "resolve"], cwd=ROOT, check=True)
    if not path.is_file():
        raise FileNotFoundError(f"Missing Sparkle tool: {name}")
    return path


def plist_settings() -> dict:
    return {
        "SUFeedURL": CONFIG["feed_url"],
        "SUPublicEDKey": PUBLIC_KEY,
        "SUEnableAutomaticChecks": True,
        "SUAutomaticallyUpdate": True,
        "SUEnableSystemProfiling": False,
        "SUVerifyUpdateBeforeExtraction": True,
        "SURequireSignedFeed": True,
    }


def embed_and_sign(app: Path, identity: str) -> None:
    source = ARTIFACT / "Sparkle.xcframework/macos-arm64_x86_64/Sparkle.framework"
    framework = app / "Contents/Frameworks/Sparkle.framework"
    framework.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["ditto", str(source), str(framework)], check=True)
    options = ["--timestamp", "--options", "runtime"] if identity != "-" else ["--timestamp=none"]
    # Sign inside out, retaining Sparkle's helper-specific sandbox entitlements.
    for relative in [
        "Versions/B/XPCServices/Downloader.xpc",
        "Versions/B/XPCServices/Installer.xpc",
        "Versions/B/Autoupdate",
        "Versions/B/Updater.app",
        "",
    ]:
        subprocess.run([
            "codesign", "--force", *options, "--preserve-metadata=entitlements",
            "--sign", identity, str(framework / relative),
        ], check=True)


if __name__ == "__main__":
    app = Path(sys.argv[1])
    plist = app / "Contents/Info.plist"
    info = plistlib.loads(plist.read_bytes())
    info.update(plist_settings())
    plist.write_bytes(plistlib.dumps(info))
    embed_and_sign(app, sys.argv[2])
