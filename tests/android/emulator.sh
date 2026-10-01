#!/usr/bin/env bash
# Manages a headless emulator for the Android test run.
#
#   ./emulator.sh ensure    create the AVD if missing, boot it, wait for boot
#   ./emulator.sh serial    print the adb serial it uses
#   ./emulator.sh abi       print the emulator ABI for this host
#   ./emulator.sh gst-abi   print the matching GStreamer-Android prefix
#   ./emulator.sh stop      shut it down
#
# A fixed port keeps the emulator distinguishable from any physical device that
# happens to be plugged in.
set -euo pipefail

SDK="${ANDROID_SDK:-${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android}}}"
AVD="${EMULATOR_AVD:-rn-gstreamer-tests}"
API="${EMULATOR_API:-35}"
PORT="${EMULATOR_PORT:-5556}"
SERIAL="emulator-$PORT"
BOOT_TIMEOUT="${EMULATOR_BOOT_TIMEOUT:-300}"

case "$(uname -m)" in
  arm64 | aarch64) ABI="arm64-v8a" ;;
  *) ABI="x86_64" ;;
esac

case "$ABI" in
  arm64-v8a) GST_ABI="arm64" ;;
  armeabi-v7a) GST_ABI="armv7" ;;
  x86_64) GST_ABI="x86_64" ;;
  x86) GST_ABI="x86" ;;
esac

IMAGE="system-images;android-$API;default;$ABI"

is_running () {
  adb devices | grep -qE "^$SERIAL[[:space:]]+device"
}

case "${1:-ensure}" in
  serial) echo "$SERIAL" ;;
  abi) echo "$ABI" ;;
  gst-abi) echo "$GST_ABI" ;;

  stop)
    if is_running; then
      echo "stopping $SERIAL" >&2
      adb -s "$SERIAL" emu kill >/dev/null 2>&1 || true
    fi
    ;;

  ensure)
    if is_running; then
      echo "emulator $SERIAL already running" >&2
      exit 0
    fi

    if ! "$SDK/emulator/emulator" -list-avds 2>/dev/null | grep -qx "$AVD"; then
      echo "creating AVD $AVD from $IMAGE" >&2
      if [ ! -d "$SDK/system-images/android-$API/default/$ABI" ]; then
        echo "error: system image not installed. Run:" >&2
        echo "  $SDK/cmdline-tools/latest/bin/sdkmanager \"$IMAGE\"" >&2
        exit 1
      fi
      echo no | "$SDK/cmdline-tools/latest/bin/avdmanager" create avd \
        -n "$AVD" -k "$IMAGE" --force >/dev/null
    fi

    echo "booting $AVD headless on port $PORT" >&2
    # swiftshader_indirect keeps it working on machines and CI runners with no
    # usable GPU; the suites never take a pipeline past READY.
    "$SDK/emulator/emulator" -avd "$AVD" -port "$PORT" \
      -no-window -no-audio -no-boot-anim -no-snapshot -wipe-data \
      -gpu swiftshader_indirect -accel auto \
      >/tmp/rn-gstreamer-emulator.log 2>&1 &

    adb -s "$SERIAL" wait-for-device

    deadline=$((SECONDS + BOOT_TIMEOUT))
    until [ "$(adb -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; do
      if [ "$SECONDS" -ge "$deadline" ]; then
        echo "error: emulator did not finish booting in ${BOOT_TIMEOUT}s" >&2
        tail -20 /tmp/rn-gstreamer-emulator.log >&2 || true
        exit 1
      fi
      sleep 2
    done

    echo "emulator $SERIAL booted" >&2
    ;;

  *)
    echo "usage: $0 {ensure|serial|abi|gst-abi|stop}" >&2
    exit 2
    ;;
esac
