#!/bin/bash
set -euxo pipefail

# fill $@ array with options passed to apt
set -- build-essential gcc-arm-none-eabi picolibc-arm-none-eabi libstdc++-arm-none-eabi libusb-1.0-0-dev cmake python3 gcab cppcheck

if [ "${CI:-}" = "true" ]; then
    set -- "$@" git ca-certificates
else
    # for local development, also add tools people want to use.
    set -- "$@" tio picotool
fi

set -- apt-get --update --no-install-recommends -y install "$@"

if [ "$UID" != 0 ]; then
    set -- sudo "$@"
fi
echo "Running $@"
"$@"
# clear $@
set --

echo "Fetching git dependencies"

SDK_VER=2.2.0-patched
EXTRAS_VER=2.2.0

git clone --branch ${SDK_VER} --depth 1 https://source.mnt.re/reform/pico-sdk || true
# Need submodules as otherwise USB will silently fail.
(cd pico-sdk && git checkout ${SDK_VER} && git submodule update --init)

git clone --branch sdk-${EXTRAS_VER} --depth 1 https://github.com/raspberrypi/pico-extras || true
(cd pico-extras && git checkout sdk-${EXTRAS_VER})
