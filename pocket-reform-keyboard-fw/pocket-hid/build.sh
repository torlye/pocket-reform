#!/bin/sh

set -exu

cmake -B build -S. -DFAMILY=rp2040
cmake --build build
