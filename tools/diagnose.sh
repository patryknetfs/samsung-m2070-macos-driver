#!/bin/bash
# Collect everything needed to debug "scanner not detected".
#   tools/diagnose.sh            -> writes ~/Desktop/m2070-diag.txt
# Read-only: changes nothing on the system.

OUT="${1:-$HOME/Desktop/m2070-diag.txt}"
DRV="/Library/Image Capture/Devices/Samsung Scanner.app"
BIN="$DRV/Contents/MacOS/Samsung Scanner"

# run a command for at most N seconds (macOS has no `timeout`)
for_secs() {
    local secs=$1; shift
    "$@" & local pid=$!
    sleep "$secs"
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}

section() { printf '\n===== %s =====\n' "$*"; }

{
section "system"
sw_vers; uname -m; date

section "installed drivers"
ls -la "/Library/Image Capture/Devices/" 2>&1
ls -la "$HOME/Library/Image Capture/Devices/" 2>&1
file "$BIN" 2>&1
codesign -dv "$DRV" 2>&1
xattr -lr "$DRV" 2>&1 | head -20
otool -L "$BIN" 2>&1
ls -d /System/Library/Frameworks/ICADevices.framework 2>&1

section "driver binary run directly (should print usage or exit quietly)"
for_secs 3 "$BIN" 2>&1
echo "exit status: $?"

section "image capture processes"
ps axo pid,user,comm | grep -iE 'icdd|imagecapture|icd|scanner' | grep -v grep

section "Bonjour: _scanner._tcp"
for_secs 6 dns-sd -Z _scanner._tcp local 2>&1
section "Bonjour: _uscan._tcp (AirScan)"
for_secs 4 dns-sd -Z _uscan._tcp local 2>&1
section "Bonjour: _ipp._tcp"
for_secs 4 dns-sd -Z _ipp._tcp local 2>&1
section "Bonjour: _pdl-datastream._tcp"
for_secs 4 dns-sd -Z _pdl-datastream._tcp local 2>&1

section "printer queues"
lpstat -v 2>&1

section "port 9400 reachability"
hosts=$( { lpstat -v 2>/dev/null; for_secs 5 dns-sd -Z _scanner._tcp local 2>/dev/null; for_secs 4 dns-sd -Z _ipp._tcp local 2>/dev/null; } \
         | grep -oE '[A-Za-z0-9-]+\.local\.?|([0-9]{1,3}\.){3}[0-9]{1,3}' | sed 's/\.$//' | sort -u)
for h in $hosts; do
    printf '%s: ' "$h"
    nc -z -G 3 "$h" 9400 >/dev/null 2>&1 && echo "9400 open" || echo "9400 closed/unreachable"
done

section "USB devices (Samsung 04e8)"
ioreg -p IOUSB -l -w0 2>/dev/null | grep -iE '"(USB Product Name|idVendor|idProduct|locationID|kUSBVendorString)"' | paste - - - - - 2>/dev/null | grep -i -E 'samsung|1256|0x4e8'
system_profiler SPUSBDataType 2>/dev/null | grep -i -B2 -A12 samsung

section "recent logs (icdd / driver), last 30 min"
log show --last 30m --style compact \
    --predicate 'process == "icdd" OR subsystem == "pl.m2070.scanner" OR process CONTAINS "Samsung" OR eventMessage CONTAINS[c] "Samsung Scanner"' 2>&1 | tail -300

section "crash reports"
ls -la ~/Library/Logs/DiagnosticReports/ /Library/Logs/DiagnosticReports/ 2>/dev/null | grep -iE 'samsung|icdd|scanner'
} > "$OUT" 2>&1

echo "done: $OUT"
