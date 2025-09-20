#!/bin/bash

# keyboard
cppcheck --check-level=exhaustive -D USBD_DESC_STR_MAX=40 -DHID_REPORT_ID= pocket-reform-keyboard-fw/pocket-hid/src

# system controller
cppcheck --check-level=exhaustive -D USBD_DESC_STR_MAX=40 pocket-reform-sysctl-fw/src
