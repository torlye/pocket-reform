#!/bin/bash

#export PICO_SDK_PATH=/usr/src/pico-sdk
#if [ -z "$MNTRE_FIRMWARE_VERSION" ]; then
#    export MNTRE_FIRMWARE_VERSION=$(date +%Y%m%d)
#fi

cmake -B build -S. -DPICO_BOARD=pico2
cmake --build build
