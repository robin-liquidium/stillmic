# NoNoise

A menu bar app that removes background noise from your microphone. Nothing else.

Other apps select the **NoNoise Mic** input. NoNoise runs only while an app is reading that input, and uses no audio processing otherwise.

Built for Apple Silicon, tuned for M4 (`-mcpu=apple-m4` in `Package.swift`). Fork of [NoNoise-Mac](https://github.com/ivalsaraj/NoNoise-Mac).

```
mic -> Core Audio callback -> DeepFilterNet3 (C) -> ring buffer -> NoNoise Mic driver -> your app
```

- `Sources/CDFN/dfn.c`: DeepFilterNet3 inference, hand-ported from the official model.
- `Sources/CDFN/pipeline.c`: real-time audio path. No allocation or locks in the audio callbacks.
- `Sources/NoNoiseMac/main.swift`: device handling and the menu.
- `Driver/`: the virtual mic (AudioServerPlugIn), unchanged from upstream.
- `Resources/dfn3.weights`: official DeepFilterNet3 weights, flattened by `tools/export_weights.py`.

## Install

```sh
./build-driver.sh && sudo ./install-driver.sh   # first time only; restarts coreaudiod
./install-app.sh
```

Then pick **NoNoise Mic** as the microphone in your call app.

## Modes

**Standard** keeps the voice smoother. **Aggressive** cuts harder and uses less CPU in quiet rooms. Switch from the menu.

## Checking the engine

`bench/dftract` runs the reference Rust implementation. `bench/dfn_bench.c` runs this one. `bench/gold.py` compares their output (run `dfn_bench` with `--aggressive`, which uses the reference's thresholds), which should differ by at most a few 16-bit steps. Run this after any change to `dfn.c`.

## Credits

[DeepFilterNet](https://github.com/Rikorose/DeepFilterNet) by Hendrik Schröter (MIT/Apache-2.0) for the model and weights. NoNoise-Mac by ivalsaraj (MIT) for the driver.
