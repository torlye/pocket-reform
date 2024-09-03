#!/bin/bash

sudo apt install build-essential pkg-config libusb-1.0-0-dev cmake gcc-arm-none-eabi tio picotool

git clone --depth 1 https://github.com/raspberrypi/pico-sdk
cd pico-sdk
git submodule init
git submodule update --depth 1
cd ..
git clone --depth 1 https://github.com/raspberrypi/pico-extras
git clone --depth 1 https://github.com/hathach/tinyusb

