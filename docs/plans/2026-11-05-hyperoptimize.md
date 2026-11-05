# Hyperoptimize NoNoise for M4 Pro

Goal: microphone noise removal only, with the lowest CPU and RAM possible on an M4 Pro. Everything else was removed.

Base commit: `b5844a8` (v1.5.1).

## What was built

- `Sources/CDFN/dfn.c`: DeepFilterNet3 hand-ported to C, using official weights. No CoreML.
- `Sources/CDFN/pipeline.c`: Core Audio callbacks only, no `AVCaptureSession` or `AVAudioEngine`.
- `Sources/NoNoiseMac/main.swift`: AppKit status item with a menu built only when opened.
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

## Not yet tested

- A real call app. Live tests used a recorder reading NoNoise Mic, with speech played through the laptop speakers.
- Bluetooth and USB mics, sample-rate conversion, device switching and `coreaudiod` restarts at runtime.
- Real room noise and listening.
- Energy use (`powermetrics` needs sudo).
- Whether BLAS stays allocation-free inside the audio callback. Threading is forced single and the path is warmed up off-thread, but this is not proven.

## Independent review

GPT-6.1 Sol reviewed the code statically. All six findings were addressed: input format and multi-buffer handling, `coreaudiod` restart recovery, actual buffer sizes and ring sizing, aggregate and virtual device exclusion, retry after a failed start, and BLAS threading and warm-up.

## Decisions

- Minimum macOS is 15, for `BLASSetThreading`.
- Compiled with `-mcpu=apple-m4`, so older chips aren't a target.
- Only the first two input channels are mixed, so an interface whose mic is on a later channel would need a channel picker.
