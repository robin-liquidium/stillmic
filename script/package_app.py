#!/usr/bin/env python3
"""Build and sign the arm64 Stillmic application bundle, with its virtual mic driver inside."""

import json
import os
import plistlib
import shutil
import subprocess
from pathlib import Path
from sparkle import embed_and_sign, plist_settings

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "dist" / "release"
APP = OUT / "Stillmic.app"
DEVELOPER_ID = "Developer ID Application: Robin Obermaier (5S5288W3R7)"
MINIMUM_MACOS = "15.0"


def run(*args: object, capture: bool = False) -> str:
    environment = os.environ.copy()
    stable_xcode = Path("/Applications/Xcode.app/Contents/Developer")
    if stable_xcode.exists():
        environment.setdefault("DEVELOPER_DIR", str(stable_xcode))
    result = subprocess.run(
        list(map(str, args)),
        check=True,
        cwd=ROOT,
        env=environment,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout or ""


def signing_options(identity: str) -> list:
    return ["--timestamp", "--options", "runtime"] if identity != "-" else ["--timestamp=none"]


def build_driver(resources: Path, identity: str) -> Path:
    source = ROOT / "Driver" / "Stillmic"
    driver = resources / "Stillmic.driver"
    binary = driver / "Contents" / "MacOS" / "Stillmic"
    binary.parent.mkdir(parents=True)
    shutil.copy2(source / "Info.plist", driver / "Contents" / "Info.plist")
    run(
        "clang", "-bundle", "-std=c11", "-O2", "-arch", "arm64", f"-mmacosx-version-min={MINIMUM_MACOS}",
        "-framework", "CoreAudio", "-framework", "CoreFoundation", "-framework", "AudioToolbox",
        source / "Stillmic.c", source / "nn_ring.c", source / "nn_clock.c", "-o", binary,
    )
    # Sign last: any later edit invalidates the signature and coreaudiod silently refuses to load it.
    run("codesign", "--force", *signing_options(identity), "--sign", identity, driver)
    run("codesign", "--verify", "--strict", driver)
    return driver


def package() -> Path:
    metadata = json.loads((ROOT / "release.json").read_text())
    identity = os.environ.get("SIGNING_IDENTITY", "-")
    build = ["swift", "build", "-c", "release", "--arch", "arm64"]
    run(*build)
    binary_dir = Path(run(*build, "--show-bin-path", capture=True).strip())

    shutil.rmtree(APP, ignore_errors=True)
    resources = APP / "Contents" / "Resources"
    resources.mkdir(parents=True)
    executable = APP / "Contents" / "MacOS" / "Stillmic"
    executable.parent.mkdir(parents=True)
    shutil.copy2(binary_dir / "Stillmic", executable)
    run("strip", "-x", executable)
    if run("lipo", "-archs", executable, capture=True).split() != ["arm64"]:
        raise RuntimeError("Expected an arm64 executable")

    for name in ["dfn3.weights", "StillmicIcon.icns"]:
        shutil.copy2(ROOT / "Resources" / name, resources / name)
    for name in ["LICENSE", "THIRD_PARTY_NOTICES.md"]:
        shutil.copy2(ROOT / name, resources / name)

    info = {
        "CFBundleExecutable": "Stillmic",
        "CFBundleIdentifier": "build.robin.stillmic",
        "CFBundleName": "Stillmic",
        "CFBundleDisplayName": "Stillmic",
        "CFBundlePackageType": "APPL",
        "CFBundleIconFile": "StillmicIcon",
        "CFBundleShortVersionString": metadata["version"],
        "CFBundleVersion": str(metadata["build"]),
        "LSArchitecturePriority": ["arm64"],
        "LSMinimumSystemVersion": MINIMUM_MACOS,
        "LSRequiresNativeExecution": True,
        "LSUIElement": True,
        "NSPrincipalClass": "NSApplication",
        "NSMicrophoneUsageDescription": "Stillmic reads your microphone to remove background noise before other apps hear it.",
    }
    info.update(plist_settings())
    with (APP / "Contents" / "Info.plist").open("wb") as output:
        plistlib.dump(info, output)

    # Inside out: driver and Sparkle first, the app last.
    build_driver(resources, identity)
    embed_and_sign(APP, identity)
    run("codesign", "--force", *signing_options(identity), "--sign", identity, "--entitlements", ROOT / "entitlements.plist", APP)
    run("codesign", "--verify", "--deep", "--strict", APP)
    if identity != "-" and identity != DEVELOPER_ID:
        print(f"Warning: signed with non-default identity: {identity}")
    return APP


if __name__ == "__main__":
    print(package())
