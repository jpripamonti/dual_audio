# Dual Audio

Dual Audio is a macOS prototype that exposes a virtual output device named `Dual Audio` and mirrors audio from that virtual device to two selected physical outputs.

The current repository is oriented toward local development and manual validation on macOS.

## Features

- **Multi-Output Mirroring**: Send audio simultaneously to two separate physical devices from a single virtual output.
- **Premium UI**: Modern macOS interface featuring glassmorphism, dynamic animations, and interactive controls.
- **Volume & Synchronization**: Fine-grained volume controls and synchronization adjustments for multiple outputs.
- **Robust Audio Recovery**: Built-in monitoring and recovery system to handle device disconnections and system wake events seamlessly.

## Installation

1. Download the latest `DualAudio.pkg` from the [Releases page](https://github.com/jpripamonti/dual_audio/releases).
2. Open the downloaded package and follow the installation instructions.
3. *Note: Since this is a prototype, you may need to right-click the `.pkg` file and select "Open" to bypass the macOS Gatekeeper warning.*

## Requirements

- macOS 14 or newer
- Xcode command line tools
- `swift`, `clang`, `codesign`, `pkgbuild`, and `productbuild` available in the shell
- Administrator access to install the HAL plug-in

## Build

Build the driver and the companion app:

```sh
make all
```

Build only the virtual audio driver:

```sh
make driver
```

Build only the app:

```sh
make app
```

## Driver installation

The development install flow is intentionally simple: install the HAL driver directly with `make`.

Install the virtual device:

```sh
sudo make install
```

Remove the virtual device:

```sh
sudo make uninstall
```

The install target copies `DualAudioDriver.driver` into `/Library/Audio/Plug-Ins/HAL` and restarts `coreaudiod` so the `Dual Audio` device becomes visible in macOS audio settings.

## Installer package

To build a local installer package that contains both the app and the driver:

```sh
make pkg
```

This produces `build/DualAudio.pkg`.

`make pkg` is for local validation. It uses ad-hoc signing and is not suitable
for public distribution.


## Repository layout

- `DualAudioDriver/`: Core Audio HAL plug-in that exposes the virtual output device
- `AudioBridge/`: low-level bridge that reads the shared ring buffer and fans out audio to one or two outputs
- `DualAudio/`: SwiftUI companion app
- `scripts/distribution.xml`: installer package distribution definition

## Current status

This codebase has implemented core functionality including stable audio recovery and a modern SwiftUI interface. Packaging and App Store readiness should be treated as active engineering work rather than finished product behavior.
