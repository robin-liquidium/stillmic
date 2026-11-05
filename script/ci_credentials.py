#!/usr/bin/env python3
"""Load GitHub Actions signing secrets into an ephemeral keychain."""

import base64
import os
import secrets
import subprocess
from pathlib import Path

REQUIRED = [
    "MACOS_CERTIFICATE_P12_BASE64",
    "MACOS_CERTIFICATE_PASSWORD",
    "APP_STORE_CONNECT_KEY_P8_BASE64",
    "APP_STORE_CONNECT_KEY_ID",
    "APP_STORE_CONNECT_ISSUER_ID",
]

for key in REQUIRED:
    if not os.environ.get(key):
        raise SystemExit(f"Missing Actions secret: {key}")

os.umask(0o077)
temp = Path(os.environ["RUNNER_TEMP"])
p12 = temp / "stillmic.p12"
p8 = temp / "stillmic.p8"
p12.write_bytes(base64.b64decode(os.environ[REQUIRED[0]]))
p8.write_bytes(base64.b64decode(os.environ[REQUIRED[2]]))
keychain = temp / "stillmic-signing.keychain-db"
password = secrets.token_hex(24)


def security(*args: object) -> None:
    result = subprocess.run(
        ["security", *map(str, args)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if result.returncode:
        raise SystemExit(f"Keychain setup failed at {args[0]}")


security("create-keychain", "-p", password, keychain)
security("set-keychain-settings", "-lut", "21600", keychain)
security("unlock-keychain", "-p", password, keychain)
security(
    "import",
    p12,
    "-k",
    keychain,
    "-P",
    os.environ["MACOS_CERTIFICATE_PASSWORD"],
    "-T",
    "/usr/bin/codesign",
    "-T",
    "/usr/bin/security",
)
security("set-key-partition-list", "-S", "apple-tool:,apple:,codesign:", "-s", "-k", password, keychain)
security("list-keychains", "-d", "user", "-s", keychain, Path.home() / "Library/Keychains/login.keychain-db")

with open(os.environ["GITHUB_ENV"], "a") as environment:
    environment.write(f"NOTARY_KEY_PATH={p8}\n")

