#!/bin/bash

# keyboard
cppcheck --error-exitcode=1 --check-level=exhaustive -D USBD_DESC_STR_MAX=40 -DHID_REPORT_ID= pocket-reform-keyboard-fw/pocket-hid/src

# system controller
cppcheck --error-exitcode=1 --check-level=exhaustive -D USBD_DESC_STR_MAX=40 pocket-reform-sysctl-fw/src
