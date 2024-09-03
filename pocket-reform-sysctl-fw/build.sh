#!/bin/bash

export PICO_SDK_PATH=$PWD/lib/pico-sdk
export PICO_EXTRAS_PATH=$PWD/lib/pico-extras

mkdir -p build
cd build
cmake ..

make
