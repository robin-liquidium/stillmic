---
name: stillmic-release
description: Ship Stillmic as a signed, notarized, Apple Silicon macOS app with resumable GitHub release automation. Use when asked to release or publish Stillmic.
---

# Stillmic release

Read `RELEASING.md`, `release.json`, and the scripts under `script/` before running a release. The repository is `robin-liquidium/stillmic`. It is an independent public repository, not a GitHub fork. Never publish Stillmic artifacts to the superseded Tab Switcher repository.

A request to release authorizes the release commit, push, immutable version tag, GitHub draft, Apple notarization, and final GitHub publication. Preserve unrelated work and never move a published tag. Releases include a signed Sparkle appcast generated from the notarized DMG; the `SPARKLE_PRIVATE_KEY` Actions secret and `sparkle.json` public key must match.

1. Inspect the working tree, remote `main`, tags, Actions runs, releases, and drafts. Resume an existing draft before creating another. Review the actual diff since the previous release.
2. Pick the next semantic version and a strictly increasing integer build. Update `release.json` with the current date and concrete user-facing changes, then run `python3 script/prepare_release.py`.
3. Run `swift test`, compile-check the Python scripts, and build with `python3 script/package_app.py`. Verify the app is arm64, has macOS 15 as its minimum, contains the signed Stillmic.driver and icon and required resources, carries only the audio-input entitlement, and has matching version/build metadata.
4. Commit and push `main`. Wait for Checks on that exact commit, including `swift test`, Python tests, and `package_app.py`. Create and push one annotated `vVERSION` tag only after CI succeeds.
5. Let the Release workflow advance the saved draft and Apple submission IDs. Never rebuild or resubmit a pending artifact. A green workflow that leaves a draft or notarization pending is not a completed release.
6. After publication, download every public asset into a fresh temporary directory. Verify `SHA256SUMS`, staples, Gatekeeper, signatures, architectures, versions, release evidence, both stable latest-download URLs, and the Sparkle appcast's archive URL and Ed25519 enclosure signature against the app's embedded public key.
7. Do not replace Robin's installed Stillmic during routine release validation unless explicitly requested. Report the public release URL, verified artifacts, and any concrete remaining action honestly.
