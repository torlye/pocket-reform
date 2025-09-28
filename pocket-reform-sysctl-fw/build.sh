#!/bin/sh

set -exu

cmake -B build -S.
cmake --build build
