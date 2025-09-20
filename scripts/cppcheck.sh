#!/bin/bash

set -ex

# keyboard
cppcheck --suppress=constParameterPointer --enable=warning --enable=style --error-exitcode=1 --check-level=exhaustive -D USBD_DESC_STR_MAX=40 -DHID_REPORT_ID= pocket-reform-keyboard-fw/pocket-hid/src

# system controller
cppcheck --suppress=badBitmaskCheck --suppress=constParameterPointer --enable=warning --enable=style --error-exitcode=1 --check-level=exhaustive -D USBD_DESC_STR_MAX=40 pocket-reform-sysctl-fw/src
