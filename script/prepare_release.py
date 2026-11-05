#!/usr/bin/env python3
"""Validate release metadata and synchronize the README release summary."""

import datetime
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def prepare() -> None:
    release = json.loads((ROOT / "release.json").read_text())
    if not re.fullmatch(r"\d+\.\d+\.\d+", release.get("version", "")):
        raise ValueError("release.json version must use semantic versioning")
    if type(release.get("build")) is not int or release["build"] < 1:
        raise ValueError("release.json build must be a positive integer")
    datetime.date.fromisoformat(release.get("date", ""))
    changes = release.get("changes")
    if not changes or not all(isinstance(change, str) and change.strip() for change in changes):
        raise ValueError("release.json changes must contain user-facing release notes")

    readme = ROOT / "README.md"
    text = readme.read_text()
    start = "<!-- release:start -->"
    end = "<!-- release:end -->"
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError("README.md must contain exactly one release marker pair")
    replacement = (
        f"{start}\n### Latest release: {release['version']}\n\n"
        + "\n".join(f"- {change}" for change in changes)
        + f"\n{end}"
    )
    readme.write_text(text[: text.index(start)] + replacement + text[text.index(end) + len(end) :])


if __name__ == "__main__":
    prepare()

