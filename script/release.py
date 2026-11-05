#!/usr/bin/env python3
"""Resume a tagged Stillmic release from exact artifacts and Apple submission IDs."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from xml.etree import ElementTree
from pathlib import Path
from typing import Optional

from package_app import APP, OUT, ROOT, package
from sparkle import tool

REPO = "robin-liquidium/stillmic"
BASE = f"https://github.com/{REPO}/releases"


def call(*args: object, input_text: Optional[str] = None) -> str:
    return subprocess.run(
        list(map(str, args)),
        cwd=ROOT,
        check=True,
        text=True,
        input=input_text,
        stdout=subprocess.PIPE,
    ).stdout.strip()


def github(*args: object) -> str:
    return call("gh", *args)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def upload(tag: str, path: Path) -> None:
    github("release", "upload", tag, path, "--clobber", "-R", REPO)


def fetch(tag: str, name: str, checksum: Optional[str] = None) -> Path:
    github("release", "download", tag, "--pattern", name, "--dir", OUT, "--clobber", "-R", REPO)
    path = OUT / name
    if checksum and sha(path) != checksum:
        raise ValueError(f"Artifact hash mismatch: {name}")
    return path


def lookup(tag: str) -> Optional[dict]:
    pages = json.loads(github("api", "--paginate", "--slurp", f"repos/{REPO}/releases?per_page=100"))
    releases = [release for page in pages for release in page]
    if any(release["draft"] and release["tag_name"] != tag for release in releases):
        raise ValueError("Another draft is pending; finish it before starting a new release")
    return next((release for release in releases if release["tag_name"] == tag), None)


def save(tag: str, state: dict) -> None:
    path = OUT / "release-state.json"
    path.write_text(json.dumps(state, indent=2) + "\n")
    upload(tag, path)


def notary(*args: object) -> dict:
    if os.environ.get("NOTARY_PROFILE"):
        auth = ["--keychain-profile", os.environ["NOTARY_PROFILE"]]
    else:
        auth = [
            "--key",
            os.environ["NOTARY_KEY_PATH"],
            "--key-id",
            os.environ["APP_STORE_CONNECT_KEY_ID"],
            "--issuer",
            os.environ["APP_STORE_CONNECT_ISSUER_ID"],
        ]
    return json.loads(call("xcrun", "notarytool", *args, *auth, "--output-format", "json"))


def submit(tag: str, state: dict, kind: str, path: Path) -> None:
    state.update(phase=f"{kind}_submitting")
    state[kind] = {"file": path.name, "sha256": sha(path)}
    save(tag, state)
    result = notary("submit", path)
    state[kind]["id"] = result["id"]
    state["phase"] = f"{kind}_pending"
    save(tag, state)


def accepted(tag: str, state: dict, kind: str) -> bool:
    result = notary("info", state[kind]["id"])
    status = result["status"]
    print(f"{kind}: {status}", flush=True)
    if status == "In Progress":
        return False
    if status != "Accepted":
        state["phase"] = f"{kind}_rejected"
        save(tag, state)
        raise RuntimeError(f"Apple {status}: inspect notarytool log {state[kind]['id']}")
    return True


def validate(path: Path, kind: str) -> None:
    call("xcrun", "stapler", "staple", path)
    call("xcrun", "stapler", "validate", path)
    if kind == "app":
        call("codesign", "--verify", "--deep", "--strict", path)
        call("spctl", "--assess", "--type", "execute", "--verbose=2", path)
    else:
        call("hdiutil", "verify", path)
        call("spctl", "--assess", "--type", "open", "--context", "context:primary-signature", "--verbose=2", path)


def archive_app(path: Path) -> None:
    path.unlink(missing_ok=True)
    call("ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", APP, path)


def generate_appcast(tag: str, versioned_dmg: Path, release: dict) -> Path:
    key = os.environ.get("SPARKLE_PRIVATE_KEY")
    if not key:
        raise ValueError("Missing SPARKLE_PRIVATE_KEY; refusing to publish an unsigned update feed")
    with tempfile.TemporaryDirectory(prefix="stillmic-appcast-") as directory:
        archives = Path(directory)
        shutil.copy2(versioned_dmg, archives / versioned_dmg.name)
        notes = archives / f"{versioned_dmg.stem}.md"
        notes.write_text("\n".join(f"- {change}" for change in release["changes"]) + "\n")
        call(
            tool("generate_appcast"),
            "--ed-key-file",
            "-",
            "--download-url-prefix",
            f"{BASE}/download/{tag}/",
            "--embed-release-notes",
            "--maximum-deltas",
            "0",
            "--maximum-versions",
            "1",
            archives,
            input_text=key,
        )
        generated = archives / "appcast.xml"
        if not generated.is_file():
            raise RuntimeError("Sparkle did not generate appcast.xml")
        call(tool("sign_update"), "--ed-key-file", "-", "--verify", generated, input_text=key)
        root = ElementTree.parse(generated).getroot()
        sparkle = "http://www.andymatuschak.org/xml-namespaces/sparkle"
        enclosure = root.find(".//enclosure")
        if enclosure is None or not enclosure.get(f"{{{sparkle}}}edSignature"):
            raise RuntimeError("Generated appcast has no Sparkle EdDSA signature")
        expected = f"{BASE}/download/{tag}/{versioned_dmg.name}"
        if enclosure.get("url") != expected:
            raise RuntimeError(f"Unexpected appcast download URL: {enclosure.get('url')}")
        feed = OUT / "appcast.xml"
        shutil.copy2(generated, feed)
        return feed


def publish_artifacts(tag: str, state: dict, release: dict) -> None:
    submitted_dmg = fetch(tag, state["dmg"]["file"], state["dmg"]["sha256"])
    validate(submitted_dmg, "dmg")
    versioned_dmg = OUT / f"Stillmic-{release['version']}.dmg"
    stable_dmg = OUT / "Stillmic.dmg"
    shutil.copy2(submitted_dmg, versioned_dmg)
    shutil.copy2(versioned_dmg, stable_dmg)

    versioned_zip = fetch(tag, state["app_zip"]["file"], state["app_zip"]["sha256"])
    stable_zip = OUT / "Stillmic.zip"
    shutil.copy2(versioned_zip, stable_zip)

    feed = generate_appcast(tag, versioned_dmg, release)

    evidence = OUT / "notarization.json"
    evidence.write_text(
        json.dumps(
            {"commit": state["commit"], "app": state["app"], "dmg": state["dmg"], "status": "Accepted"},
            indent=2,
        )
        + "\n"
    )
    files = [versioned_dmg, stable_dmg, versioned_zip, stable_zip, feed, evidence, ROOT / "release.json"]
    checksums = OUT / "SHA256SUMS"
    checksums.write_text("".join(f"{sha(path)}  {path.name}\n" for path in files))
    files.append(checksums)
    for path in files:
        upload(tag, path)
    state["final"] = {path.name: sha(path) for path in files}
    state["phase"] = "ready"
    save(tag, state)


def advance(tag: str) -> None:
    if not re.fullmatch(r"v\d+\.\d+\.\d+", tag):
        raise ValueError("Use vMAJOR.MINOR.PATCH")
    OUT.mkdir(parents=True, exist_ok=True)
    release = json.loads((ROOT / "release.json").read_text())
    commit = call("git", "rev-parse", "HEAD")
    if tag != "v" + release["version"] or call("git", "rev-list", "-n", "1", tag) != commit:
        raise ValueError("Checkout, tag, and release version must agree")
    if call("git", "status", "--porcelain"):
        raise ValueError("Release requires a clean checkout")
    if not os.environ.get("SPARKLE_PRIVATE_KEY"):
        raise ValueError("Missing SPARKLE_PRIVATE_KEY; refusing to build an update-enabled release")
    call("git", "fetch", "origin", "main")
    call("git", "merge-base", "--is-ancestor", commit, "origin/main")

    current = lookup(tag)
    if current and not current["draft"]:
        print("Already published: " + current["html_url"])
        return
    if not current:
        notes = OUT / "notes.md"
        notes.write_text(
            "\n".join(f"- {change}" for change in release["changes"])
            + "\n\nRequires macOS 15 or later on Apple Silicon.\n"
        )
        github(
            "release",
            "create",
            tag,
            "--verify-tag",
            "--draft",
            "--title",
            f"Stillmic {release['version']}",
            "--notes-file",
            notes,
            "-R",
            REPO,
        )
        current = lookup(tag)
        if not current:
            print("Draft created; rerun after GitHub finishes indexing it.")
            return

    if any(asset["name"] == "release-state.json" for asset in current["assets"]):
        state = json.loads(fetch(tag, "release-state.json").read_text())
        if state["commit"] != commit or state["version"] != release["version"]:
            raise ValueError("Draft belongs to another commit")
    else:
        state = {"commit": commit, "version": release["version"], "phase": "build"}
        save(tag, state)

    if state["phase"].endswith(("_submitting", "_rejected")):
        raise RuntimeError("Reconcile Apple state before continuing: " + state["phase"])
    if state["phase"] == "build":
        if not os.environ.get("SIGNING_IDENTITY") or os.environ["SIGNING_IDENTITY"] == "-":
            raise ValueError("Developer ID signing is required")
        package()
        submission = OUT / "submission-app.zip"
        archive_app(submission)
        upload(tag, submission)
        submit(tag, state, "app", submission)
    if state["phase"] == "app_pending":
        if not accepted(tag, state, "app"):
            return
        source = fetch(tag, state["app"]["file"], state["app"]["sha256"])
        shutil.rmtree(APP, ignore_errors=True)
        call("ditto", "-x", "-k", source, OUT)
        validate(APP, "app")
        app_zip = OUT / f"Stillmic-{release['version']}.zip"
        archive_app(app_zip)
        upload(tag, app_zip)
        state["app_zip"] = {"file": app_zip.name, "sha256": sha(app_zip)}

        staging = OUT / "dmg-stage"
        shutil.rmtree(staging, ignore_errors=True)
        staging.mkdir()
        call("ditto", APP, staging / APP.name)
        (staging / "Applications").symlink_to("/Applications")
        dmg = OUT / "submission-dmg.dmg"
        dmg.unlink(missing_ok=True)
        call("hdiutil", "create", "-volname", "Stillmic", "-srcfolder", staging, "-format", "UDZO", dmg)
        call("codesign", "--timestamp", "--sign", os.environ["SIGNING_IDENTITY"], dmg)
        upload(tag, dmg)
        submit(tag, state, "dmg", dmg)
    if state["phase"] == "dmg_pending":
        if not accepted(tag, state, "dmg"):
            return
        publish_artifacts(tag, state, release)
    if state["phase"] == "ready":
        for filename, checksum in state["final"].items():
            fetch(tag, filename, checksum)
        current = lookup(tag)
        for asset in current["assets"]:
            if asset["name"] in ["submission-app.zip", "submission-dmg.dmg"]:
                github("api", f"repos/{REPO}/releases/assets/{asset['id']}", "-X", "DELETE")
        github("release", "edit", tag, "--draft=false", "--latest", "-R", REPO)
        print("Published " + BASE + "/tag/" + tag)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    advance(parser.parse_args().tag)
