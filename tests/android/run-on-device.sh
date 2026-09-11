#!/usr/bin/env bash
# Runs one GstCheck suite on an Android target. CTest invokes this per suite;
# the binary and libgstreamer_android.so come from the build's out/ directory.
#
#   TARGET=emulator (default)  boot/reuse the headless AVD from emulator.sh
#   TARGET=device              use an attached phone (ANDROID_SERIAL to pick one)
set -euo pipefail

BIN="$1"
OUT_DIR="$(cd "$(dirname "$BIN")" && pwd)"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_DIR="${DEVICE_DIR:-/data/local/tmp/rn-gstreamer-tests}"

if [ "${TARGET:-emulator}" = emulator ]; then
  "$HERE/emulator.sh" ensure
  SERIAL="$("$HERE/emulator.sh" serial)"
else
  SERIAL="${ANDROID_SERIAL:-$(adb devices | awk '$2 == "device" && $1 !~ /^emulator-/ { print $1; exit }')}"
  [ -n "$SERIAL" ] || { echo "error: no physical device attached" >&2; exit 1; }
fi

echo "==> target: $SERIAL"

adb -s "$SERIAL" shell "mkdir -p $DEVICE_DIR" >/dev/null
for so in "$OUT_DIR"/*.so; do
  adb -s "$SERIAL" push "$so" "$DEVICE_DIR/" >/dev/null
done
adb -s "$SERIAL" push "$BIN" "$DEVICE_DIR/" >/dev/null

# CK_FORK stays off on Android regardless of the host OS: fork mode truncates
# the run and takes the emulator offline. It also suits the suites, whose
# backend state is process-global.
name="$(basename "$BIN")"
adb -s "$SERIAL" shell "cd $DEVICE_DIR && chmod 755 $name && \
  LD_LIBRARY_PATH=$DEVICE_DIR CK_FORK=no GST_REGISTRY_DISABLE=yes ./$name"
