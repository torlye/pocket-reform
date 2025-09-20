#!/bin/bash
set -euxo pipefail

# fill $@ array with options passed to apt
set -- build-essential avr-libc gcc-avr gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib libusb-1.0-0-dev cmake python3 gcab cppcheck

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

git clone --branch 1.5.1 --depth 1 https://github.com/raspberrypi/pico-sdk || true
# Need submodules as otherwise USB will silently fail.
(cd pico-sdk && git checkout 1.5.1 && git submodule update --init)

git clone --branch sdk-1.5.1 --depth 1 https://github.com/raspberrypi/pico-extras || true
(cd pico-extras && git checkout sdk-1.5.1)
