#!/bin/bash

FWPATH=../pocket-reform-keyboard-fw/pocket-hid/build/pocket-hid.uf2
# press X key in the OLED menu first to enter firmware update mode (RP2 Boot)
USBDEV=2e8a:0003

# make sure the file is there
strings $FWPATH | grep PREFHID > /dev/null
if [[ $? != 0 ]]; then 
	echo "Firmware file $FWPATH is invalid, exiting."
	exit 1
fi

set -e

# make sure we have picotool
apt install -y picotool

# identify keyboard on usb bus
COUNT=$(lsusb | grep $USBDEV | wc -l)
if [[ $COUNT != 1 ]];
then echo "MNT Pocket Reform Input not found or more than one found, exiting."
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
echo "Flashing MNT Pocket Reform Input (bus $BUS, address $ADDR)..."

picotool load --bus $BUS --address $ADDR -f $FWPATH
picotool reboot -f

echo "Done."

