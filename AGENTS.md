# Stillmic

macOS menu bar app that removes background noise from the mic. Noise removal is the product: keep CPU and RAM as low as possible, and add nothing that doesn't serve that.

## Rules

- **Don't touch the audio of a running install during a call.** Replacing the app or driver, restarting `coreaudiod` or quitting Stillmic silences the user's mic. Check who reads the mic first with `bench/who.swift`.
- **Audio callbacks are real-time.** In `Sources/CDFN`, code under `mic_io`, `engine_io` and `dfn_process` must not allocate, lock, log or block. Keep it in C. Swift stays on the main thread.
- **Engine changes need the reference check.** `swift test` compares Aggressive mode against `Tests/CDFNTests/Fixtures/reference.wav`, produced by the official `deep_filter` crate (`bench/dftract`). Allowed difference is 2 16-bit steps.
- Device UIDs in `Sources/Stillmic/main.swift` and `Driver/Stillmic/Stillmic.c` must match. A mismatch fails silently.
- Change `CFBundleVersion` in `Driver/Stillmic/Info.plist` when the driver changes, so the app offers an update. Sparkle updates the app but not the installed driver.
- Sign inside out: driver, Sparkle, then the app. Any edit after signing makes `coreaudiod` silently refuse the driver.

## Commands

```sh
swift test
Driver/tests/run-tests.sh
python3 -m unittest discover -s script/tests -p 'test_*.py'
script/build_and_run.sh --build   # builds and signs only
```

Measurements and what's still untested are in `docs/measurements.md`. Releasing is in `RELEASING.md`.

## Website

`website/` is an Astro site on a Cloudflare Worker. In that folder, run `bun run lint`, `bun run check`, `bun run fmt:check` and `bun run build` before deploying with `bun run deploy`. It reads the version from `release.json`, so redeploy after a release. `src/data/demo.json` and `public/demo/` come from the real engine and shouldn't be edited by hand. Measurements are in `docs/measurements.md`.

Package scripts there run the Bun canary pinned in `package.json`, which `bun lint` needs. Prettier stays only for `.astro` files, which `bun fmt` hands to it.
