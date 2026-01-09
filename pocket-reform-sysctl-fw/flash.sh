#!/bin/bash
set -ex
sudo reform-mcu-tool bootsel pocket-sysctl-1.0
sleep 0.5
sudo picotool load build/sysctl.uf2
sudo picotool reboot

