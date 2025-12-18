#!/bin/sh

set -exu

cmake -B build -S. -DPICO_BOARD=pico2
cmake --build build
