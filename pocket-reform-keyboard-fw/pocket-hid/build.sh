#!/bin/bash

export PICO_SDK_PATH=/usr/src/pico-sdk

mkdir -p build
cd build
cmake ..

make
