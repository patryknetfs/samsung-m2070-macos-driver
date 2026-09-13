# Samsung M2070 — native arm64 macOS scanner driver

A clean-room, from-scratch Image Capture Device (ICD) driver that lets
Preview / Image Capture / third-party apps scan from a Samsung M2070
(and M2060) MultiXpress all-in-one over the network. It builds the
driver bundle natively for Apple Silicon and does **not** require the
original Samsung driver files.

The 17-year-old Samsung driver line was never released for arm64 macOS;
this project is a native reimplementation of the client side of the
SANE `xerox_mfp` protocol that the M2070 speaks.

## Why this exists

Samsung discontinued the M2070 driver (and the whole printer division,
sold to HP) long before Apple Silicon. The old x86 32-bit driver neither
installs nor runs on modern macOS. This driver restores network scanning
for this otherwise-perfectly-fine, now "obsolete" all-in-one.

## What's in the repo

```
main.c                 the driver — a native ICA scanner-ICD implementation
Makefile               builds + installs the .app bundle
Samsung Scanner.app/   minimal bundle skeleton (Info.plist + scan-mode descriptors)
    Contents/Info.plist
    Contents/Resources/DeviceInfo.plist          slim: M2070/M2060 only
    Contents/Resources/DeviceMatchingInfo.plist  slim: M2070/M2060 only
```

The resource plists have been trimmed to just the M20x0 family; the
full Samsung `DeviceInfo/`/`DeviceMatchingInfo` plists (covering 100+
obsolete models) were intentionally **not** redistributed.

## Requirements

- macOS 13.0+ on Apple Silicon
- Xcode Command Line Tools (`xcode-select --install`)

## Build

```sh
make
```

Builds `Samsung Scanner.app` in place and ad-hoc codesigns it.

## Install

```sh
sudo make install
```

This stops the ICA daemon, replaces
`/Library/Image Capture/Devices/Samsung Scanner.app` with the freshly
built bundle, and restarts the daemon. Then:

1. Make sure the M2070 is on your network.
2. Open **Preview → File → Import from Scanner** (or Image Capture).
3. The scanner appears as **Samsung M2070 Series**.

## Uninstall

```sh
sudo rm -rf "/Library/Image Capture/Devices/Samsung Scanner.app"
sudo killall iccd   # restarts the daemon
```

## How the driver works

The driver registers with Image Capture via the ICA SDK, converts the
app's scan requests into the SANE `xerox_mfp` wire protocol over
TCP/9400 (the M2070's scan interface), and pipes decoded image data
back as TIFF. It handles grayscale and color, document-feeder and flatbed
through their respective image-processing paths.

See the explanatory comments and the `SetupScanner`/`ProcessScan`
functions in `main.c` for the details.

## License

BSD 2-Clause. See [LICENSE](LICENSE).

## Acknowledgements

The reverse engineer lives on a farm. The SANE project's `xerox_mfp`
backends and the open-source `wia`/scanner hacking community made this
possible. Not affiliated with Samsung or HP.