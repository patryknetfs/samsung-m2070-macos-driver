#!/bin/bash
# Add a Samsung M2070 / M2070W / M2070FW print queue on macOS (Apple Silicon).
#
#   tools/setup-printer.sh wifi [IP]   driverless AirPrint / IPP Everywhere
#   tools/setup-printer.sh usb         USB queue (needs the Samsung/HP PPD)
#
# Printing is not part of the scanner driver: the M2070 only speaks Samsung's
# host-based SPL language, which macOS cannot render by itself.  Over the
# network the printer's AirPrint (IPP) service does the rendering, so no
# driver is needed.  Over USB there is no AirPrint, so the queue needs the
# vendor PPD + SPL filter from HP's "Samsung Printer Driver" package.

set -euo pipefail

QUEUE="${QUEUE:-Samsung_M2070}"
PPD_DIR="/Library/Printers/PPDs/Contents/Resources"

die() { echo "error: $*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"
}

wifi() {
    local ip="${1:-}" uri=""
    if [ -z "$ip" ]; then
        echo "== looking for the printer via Bonjour (_ipp._tcp) =="
        uri=$(ippfind -T 8 _ipp._tcp --name 'M20[67]0' 2>/dev/null | head -1 || true)
        [ -n "$uri" ] || die "printer not found on the network; pass its IP: $0 wifi 192.168.1.50"
    else
        uri="ipp://$ip/ipp/print"
    fi
    echo "== using $uri =="
    # IPP Everywhere / AirPrint: CUPS builds the PPD from the printer's own
    # attributes, no vendor code involved
    lpadmin -p "$QUEUE" -E -v "$uri" -m everywhere \
            -D "Samsung M2070 (AirPrint)" -o printer-is-shared=false \
        || die "the printer did not answer as AirPrint/IPP Everywhere.
       Update its firmware (HP support page for SL-M2070W) and make sure
       AirPrint/IPP is enabled in its web interface (http://${ip:-<printer-ip>}/)."
    cupsenable "$QUEUE"; cupsaccept "$QUEUE"
    echo "== queue $QUEUE ready; test: lp -d $QUEUE /System/Library/Desktop\\ Pictures/*.heic =="
}

usb() {
    local uri ppd
    uri=$(lpinfo -v 2>/dev/null | awk '/usb:\/\/Samsung\/M20[67]0/ {print $2; exit}')
    [ -n "$uri" ] || die "no Samsung M2060/M2070 visible on USB (check cable, power, 'lpinfo -v')"
    echo "== found $uri =="

    ppd=$(ls "$PPD_DIR"/Samsung\ M2070* "$PPD_DIR"/Samsung\ M2060* 2>/dev/null | head -1 || true)
    if [ -z "$ppd" ]; then
        cat >&2 <<'MSG'
error: the Samsung PPD/SPL filter is not installed.
  1. Install Rosetta 2 (the SPL filter is x86_64):  softwareupdate --install-rosetta --agree-to-license
  2. Install HP's "Samsung Printer Driver" for macOS (V3.93.x) from
     support.hp.com -> Samsung Xpress SL-M2070 -> Software and drivers.
  3. Run this script again.
Alternatively connect the printer to Wi-Fi and use:  setup-printer.sh wifi
MSG
        exit 1
    fi
    if [ "$(uname -m)" = arm64 ] && ! /usr/bin/pgrep -q oahd; then
        echo "warning: Rosetta 2 does not seem to be installed; the Samsung filter will fail." >&2
        echo "         run: softwareupdate --install-rosetta --agree-to-license" >&2
    fi
    echo "== using PPD $ppd =="
    lpadmin -p "${QUEUE}_USB" -E -v "$uri" -P "$ppd" -D "Samsung M2070 (USB)" \
            -o printer-is-shared=false
    cupsenable "${QUEUE}_USB"; cupsaccept "${QUEUE}_USB"
    echo "== queue ${QUEUE}_USB ready =="
}

case "${1:-}" in
    wifi) need_root "$@"; shift; wifi "${1:-}" ;;
    usb)  need_root "$@"; usb ;;
    *)    sed -n '2,12p' "$0"; exit 2 ;;
esac
