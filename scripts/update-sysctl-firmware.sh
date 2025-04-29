#!/bin/bash

set -e

BUILD_JOB=$1
SELF_UPDATE=0
MODEL="unknown"

if [[ $BUILD_JOB != +([0-9]) ]]; then
	echo "You must specify a MNT CI build job number to download the firmware from, for example:"
	echo
	echo "sudo ./update-sysctl-firmware.sh 5694"
	exit 1
fi

if [[ $EUID -ne 0 ]]; then
	echo "This script must be run as root, for example:"
	echo
	echo "sudo ./update-sysctl-firmware.sh 5694"
	exit 1
fi

if [[ -z /proc/device-tree/model ]]; then
	MODEL=$(cat /proc/device-tree/model)
	if [[ $MODEL =~ ".*MNT Pocket Reform.*" ]]; then
		SELF_UPDATE=1
		echo "MNT Pocket Reform as host detected, assuming you want to flash the system that"
		echo "is running this tool. If that's not the case, exit now using Ctrl+C."
		echo ""
	fi
fi

# make sure we have picotool and reform-tools (reform-mcu-tool)
DEPS="picotool reform-tools curl"

echo
echo Ensuring dependencies: $DEPS...
echo
apt update -qq
apt install -qqqy $DEPS

TMPDIR=$(mktemp -d pocket-sysctl-tmp.XXXXXXXX)
FWPATH="$TMPDIR/sysctl.uf2"
FWURL="https://source.mnt.re/reform/pocket-reform/-/jobs/$BUILD_JOB/artifacts/raw/pocket-reform-sysctl-fw/build/sysctl.uf2"

USBDEV="unknown"
USBDEV_RP2040=2e8a:000a
USBDEV_BOOTSEL=2e8a:0003
USBDEV_MNT=1209:6d07
SELF_UPDATE=0

# download the firmware file
echo
echo Downloading firmware to $FWPATH...
echo
curl "$FWURL" --output "$FWPATH"

# make sure the file is there
# TODO: can we download an MD5 hash and compare?
strings "$FWPATH" | grep PREF1SYS > /dev/null
if [[ $? != 0 ]]; then 
	echo "Firmware file $FWPATH is invalid, exiting."
	exit 1
fi

echo
echo "Firmware downloaded to $FWPATH."
echo

cleanup () {
	set +e
	# restore brightness
	brightnessctl s 100%
}

find_usb_device_ids () {
	# extract usb bus and device address numbers
	echo "USBDEV: $USBDEV"

	ROW=$(lsusb | grep $USBDEV)
	BUS=$(echo $ROW | cut -d ' ' -f 2)
	ADDR=$(echo $ROW | cut -d ' ' -f 4)
	ADDR=${ADDR//:}

	# make sure these are integers
	BUS=$(expr $BUS + 0)
	ADDR=$(expr $ADDR + 0)
	return 0
}

ask_for_confirmation () {
	echo "Ready to flash System Controller (bus $BUS, address $ADDR)."
	echo "Make sure all your work is saved--the system will power down after the process is completed."
	echo ""
	echo "Press return to continue or CTRL+C to abort."
	read
	return 0
}

is_display_panel_version_2 () {
	set +e
	local VER_FILE=$(find /sys/devices -name "mnt_pocket_reform_panel_version")
	local PANEL_VER=$(cat "$VER_FILE" || echo 0)
	set -e

	echo "Display panel version: $PANEL_VER"

	if [[ $PANEL_VER -eq 2 ]]; then
		return 0
	fi
	return 1
}

safety_for_self_update () {
	if [[ $SELF_UPDATE -eq 1 ]]; then
		if is_display_panel_version_2; then
			brightnessctl s 0
		fi
		# hack to prevent writes + fs corruption to rootfs
		# FIXME: makes filesystem read-only, how to revert?
		sync
		echo u > /proc/sysrq-trigger
	fi
	return 0
}

flash_with_mcutool () {
	echo "Method: reform-mcutool, then picotool. Self update: $SELF_UPDATE."
	USBDEV="$1"
	find_usb_device_ids
	ask_for_confirmation
	safety_for_self_update

	reform-mcu-tool bootsel pocket-sysctl-1.0
	sleep 0.5

	picotool load --bus $BUS --address $ADDR $FWPATH
	sleep 0.5
	picotool reboot
	sleep 0.5
	systemctl reboot -f
	exit 0
}

flash_with_picotool () {
	echo "Method: direct picotool. Self update: $SELF_UPDATE."
	USBDEV="$1"
	find_usb_device_ids
	ask_for_confirmation
	safety_for_self_update

	picotool load --bus $BUS --address $ADDR $FWPATH
	sleep 0.5
	picotool reboot
	sleep 0.5
	systemctl reboot -f
	exit 0
}

# identify system controller on usb bus
COUNT_MNT=$(lsusb | grep $USBDEV_MNT | wc -l)
COUNT_RP2040=$(lsusb | grep $USBDEV_RP2040 | wc -l)
COUNT_BOOTSEL=$(lsusb | grep $USBDEV_BOOTSEL | wc -l)

if [[ $COUNT_MNT -eq 1 ]];
then
	flash_with_mcutool $USBDEV_MNT
	exit 0
fi

if [[ $COUNT_BOOTSEL -eq 1 ]];
then
	flash_with_picotool $USBDEV_BOOTSEL
	exit 0
fi

if [[ $COUNT_RP2040 -eq 1 ]];
then 
	flash_with_picotool $USBDEV_RP2040
	exit 0
fi

echo "Error: MNT Pocket Reform System Controller device not found or more than one found."
exit 1


