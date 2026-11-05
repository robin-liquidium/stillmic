# Hyperoptimizing Stillmic on an M4 Pro

Goal: microphone noise removal only, with the lowest CPU and RAM possible on an M4 Pro. Everything else was removed.

Base commit: `b5844a8` (NoNoise-Mac v1.5.1), the "Stock 1.5.1" column below.

## What was built

- `Sources/CDFN/dfn.c`: DeepFilterNet3 hand-ported to C, using official weights. No CoreML.
- `Sources/CDFN/pipeline.c`: Core Audio callbacks only, no `AVCaptureSession` or `AVAudioEngine`.
- `Sources/Stillmic/main.swift`: AppKit status item with a menu built only when opened.
- The driver is unchanged, so installing the app does not restart `coreaudiod`.

## Measurements

M4 Pro on battery. 60 s test clip: text-to-speech with synthetic fan, hum and click noise. CPU is % of one core.

| Test | Stock 1.5.1 | Now |
|---|---|---|
| Offline engine CPU | about 14% | 1.12% |
| Noise left in pauses | -50.7 dBFS | -63.2 dBFS |
| SI-SDR vs clean speech | 9.26 dB | 16.38 dB |
| Live CPU, app reading NoNoise Mic, quiet room | 38-45% | 2.8% |
| Live footprint, same | 61 MB | 14.3 MB |
| App idle, no mic reader | not measured | 0.0% CPU, 14 MB |
| Latency, mic to NoNoise Mic | not measured | 80 ms (30 ms of it is the model) |

The engine's output matches the reference `deep_filter` 0.5.6 / tract to within 1 16-bit step on the test clip (-101.6 dB difference). The reference itself uses 3.9% CPU and about 42 MB.

Live CPU depends on speech: frames judged as noise only skip both decoders. While speech played it reached about 6.5%. `coreaudiod` stayed at 6.6-8.1% in a quiet room, about the same as before.

## Release build

- `-mcpu=apple-m1` instead of `-mcpu=apple-m4`: no measurable difference. Offline CPU was 1.28-1.29% vs 1.29-1.30% in Standard and 1.15-1.16% vs 1.15-1.17% in Aggressive, over 3 runs each. Most of the work is in Accelerate, which picks its own code path at runtime.
- Sparkle: 13.6 MB without the updater started, 15.2 MB with it, measured with `--no-audio` (framework linked in both). Earlier builds with no Sparkle linked measured 13.8 MB in the same mode.

## Modes

Picked by ear from level-matched clips of a real recording.

| Mode | Local SNR thresholds (noise only / clean / deep filter) | Frame-to-frame gain wobble | Speech level change |
|---|---|---|---|
| Standard (default) | -15 / 35 / 35 dB | 1.28 dB | -5.0 dB |
| Aggressive | -10 / 30 / 20 dB (libDF defaults) | 2.41 dB | -9.2 dB |

The model's estimate never goes below -15 dB, so Standard runs the decoders on every frame. Offline quiet-room CPU is 1.33% versus 0.55%. A -14 dB cutoff recovers that, but wobble rises to 1.62 dB, so it was rejected.

Live during a real call in Standard: NoNoise 6.5-7.8% CPU and 14.5 MB, `coreaudiod` 7.8-9.1%.

## Not yet tested

- Aggressive mode and mode switching live. Feedback from people on a real call.
- Bluetooth and USB mics, sample-rate conversion, device switching and `coreaudiod` restarts at runtime.
- Real room noise and listening.
- Energy use (`powermetrics` needs sudo).
- Whether BLAS stays allocation-free inside the audio callback. Threading is forced single and the path is warmed up off-thread, but this is not proven.

The signed release has not carried live audio yet. Its driver, device names and install-from-menu flow are new and need a live test: that a hardened-runtime, Developer ID signed driver loads in `coreaudiod`, and that `do shell script ... with administrator privileges` works from the hardened app with no Apple Events entitlement.

## Independent review

GPT-6.1 Sol reviewed the code statically. All six findings were addressed: input format and multi-buffer handling, `coreaudiod` restart recovery, actual buffer sizes and ring sizing, aggregate and virtual device exclusion, retry after a failed start, and BLAS threading and warm-up.

## Decisions

- Minimum macOS is 15, for `BLASSetThreading`.
- Compiled with `-mcpu=apple-m4`, so older chips aren't a target.
- Only the first two input channels are mixed, so an interface whose mic is on a later channel would need a channel picker.
