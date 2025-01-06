#!/bin/bash

export PICO_SDK_PATH=/usr/src/pico-sdk
export MNTRE_FIRMWARE_VERSION=$(date +%Y%m%d)

mkdir -p build
cd build
cmake ..

make
