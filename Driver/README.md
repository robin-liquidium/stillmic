# Stillmic Mic driver

`Stillmic.driver` is a userspace Core Audio plug-in (an AudioServerPlugIn that runs inside `coreaudiod`). It came from [NoNoise-Mac](https://github.com/ivalsaraj/NoNoise-Mac), where it was written against Apple's public `AudioServerPlugIn.h` API, and is MIT licensed. Stillmic renames it so both can be installed together.

## Devices

- **Stillmic**: visible, input only (UID `Stillmic:visible:48k2ch`). Apps select this as their microphone.
- **Stillmic Engine**: hidden, output only (UID `Stillmic:engine:48k2ch`). The app writes cleaned audio here.

Both share one loopback ring (`nn_ring`) and a per-device zero-timestamp clock (`nn_clock`) anchored to one host time. Audio is 48 kHz, stereo, interleaved Float32. The UIDs must match `Sources/Stillmic/main.swift`.

The ring returns silence for anything the app hasn't written, so a stopped app can't replay old speech.

## Tests

`Driver/tests/run-tests.sh` tests the ring and clock math without any audio device.

## Installing

The app bundles the driver and installs it from its menu. Installing restarts `coreaudiod`, so all audio on the Mac stops for a moment. Change `CFBundleVersion` in `Stillmic/Info.plist` when the driver changes, so the app offers the update.
