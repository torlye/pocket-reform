#!/bin/bash

set -ex

# keyboard
cppcheck --suppress=subtractPointers --suppress=constParameterPointer --enable=warning --enable=style --error-exitcode=1 --check-level=exhaustive -D USBD_DESC_STR_MAX=40 -DHID_REPORT_ID= pocket-reform-keyboard-fw/pocket-hid/src

# system controller
# TODO: fix 32 bit == unsigned long issue
cppcheck --suppress=badBitmaskCheck --suppress=constParameterPointer --suppress=invalidPrintfArgType_uint --enable=warning --enable=style --error-exitcode=1 --check-level=exhaustive -D USBD_DESC_STR_MAX=40 pocket-reform-sysctl-fw/src
