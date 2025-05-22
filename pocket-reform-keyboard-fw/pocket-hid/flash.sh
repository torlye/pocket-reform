#!/bin/sh

set -e

if [ "$(id -u)" -ne 0 ]; then
	echo "you need to run this as root (for example by using sudo)" >&2
	exit 1
fi

if [ ! -e build/pocket-hid.uf2 ]; then
	echo "build/pocket-hid.uf2 does not exist -- did you forget to compile?" >&2
	exit 1
fi

echo "To put your keyboard into programming mode, open the OLED menu by" >&2
echo "pressing and holding the hyper key, followed by the enter key." >&2
echo "Once you are in the oled menu, press X to enter programming mode." >&2
echo "Once you press Enter, you have 10 seconds to open the OLED menu" >&2
echo "and press X." >&2
echo "Press the Enter key once you are ready" >&2
# shellcheck disable=SC2034
read -r enter
sleep 10

picotool load build/pocket-hid.uf2 --bus 1 -f
sleep 1
picotool reboot --bus 1 -f
