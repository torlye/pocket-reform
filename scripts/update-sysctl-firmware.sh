#!/bin/bash

FWPATH=../pocket-reform-sysctl-fw/build/sysctl.uf2
# change to 2e8a:0003 if rp2040 is in "Raspberry Pi RP2 Boot" mode
USBDEV=2e8a:000a

# make sure the file is there
strings $FWPATH | grep PREF1SYS > /dev/null
if [[ $? != 0 ]]; then 
	echo "Firmware file $FWPATH is invalid, exiting."
	exit 1
fi

set -e

# make sure we have picotool
apt install -y picotool

# identify system controller on usb bus
COUNT=$(lsusb | grep $USBDEV | wc -l)
if [[ $COUNT != 1 ]];
then echo "RP2040 System Controller device not found or more than one found, exiting."
	exit 1
fi

# extract usb bus and device address numbers
ROW=$(lsusb | grep $USBDEV)
BUS=$(echo $ROW | cut -d ' ' -f 2)
ADDR=$(echo $ROW | cut -d ' ' -f 4)
ADDR=${ADDR//:}

# make sure these are integers
BUS=$(expr $BUS + 0)
ADDR=$(expr $ADDR + 0)

# ask for confirmation
echo "Ready to flash System Controller (bus $BUS, address $ADDR). Make sure all your work is saved--the system will power down after the process is completed."
echo ""
echo "Press return to continue or CTRL+C to abort."
read

# flash! (this will hard power down the system)
sync
sleep 0.5
# hack to prevent writes + fs corruption to rootfs
echo u > /proc/sysrq-trigger
sleep 0.5
picotool load --bus $BUS --address $ADDR -f $FWPATH
picotool reboot -f

