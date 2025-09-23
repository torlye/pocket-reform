#!/bin/sh

set -exu

# set MNTRE_FIRMWARE_VERSION to current date by default
: "${MNTRE_FIRMWARE_VERSION:=$(date +%Y%m%d)}"

cmake -B build -S.
cmake --build build
