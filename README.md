# Samsung M2070 — native arm64 macOS scanner driver

A clean-room, from-scratch Image Capture Device (ICD) driver that lets
Preview / Image Capture / third-party apps scan from a Samsung M2070
(and M2060) all-in-one over the network (Wi-Fi / Ethernet) **and USB**. It builds the
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
tools/setup-printer.sh adds a print queue (AirPrint over Wi-Fi, PPD over USB)
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

This replaces `/Library/Image Capture/Devices/Samsung Scanner.app` with the
freshly built bundle and restarts the Image Capture daemon (`icdd`). Then:

1. Connect the M2070 with a USB cable, or put it on the same network as the Mac.
2. Open **Preview → File → Import from Scanner** (or Image Capture).
3. The scanner appears as **Samsung M2070 Series**.

## Printing

Printing is not done by this bundle — the M2070 speaks only Samsung's
host-based SPL printer language. Use the helper:

```sh
tools/setup-printer.sh wifi [printer-ip]   # Wi-Fi/Ethernet: driverless AirPrint (recommended)
tools/setup-printer.sh usb                 # USB: needs HP's Samsung driver package + Rosetta 2
```

- **Wi-Fi / Ethernet**: the M2070W/FW have AirPrint, so CUPS creates an
  IPP Everywhere queue with no vendor code at all. If it is not found,
  update the printer firmware and enable AirPrint/IPP in its web page.
- **USB**: there is no AirPrint over USB on this model, so the queue needs
  the Samsung PPD and SPL filter from HP's "Samsung Printer Driver" (V3.93.x)
  for macOS. That filter is x86_64, so Rosetta 2 must be installed.

## Uninstall

```sh
sudo make uninstall
```

## How the driver works

The driver registers with Image Capture via the ICA SDK and converts the
app's scan requests into the SANE `xerox_mfp` wire protocol — over
TCP/9400 for network devices, or over the bulk pipes of the USB
vendor-specific interface (via IOKit) for USB devices. The finished image
is written in the format the app asked for (JPEG, TIFF, PNG, HEIC or PDF). It handles grayscale and color, document-feeder and flatbed
through their respective image-processing paths.

See the explanatory comments and the `SetupScanner`/`ProcessScan`
functions in `main.c` for the details.

## License

BSD 2-Clause. See [LICENSE](LICENSE).

## Acknowledgements

The reverse engineer lives on a farm. The SANE project's `xerox_mfp`
backends and the open-source `wia`/scanner hacking community made this
possible. Not affiliated with Samsung or HP.