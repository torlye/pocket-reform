#!/bin/bash

# NOTE: on device, if using display v2, you have to set
# the display brightness to 0% before doing this procedure,
# and can restore it afterwards. i.e.:
# brightnessctl s 0

reform-mcu-tool bootsel pocket-sysctl-1.0
sleep 0.5
picotool load build/sysctl.uf2
picotool reboot

