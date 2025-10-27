# Development on the Pocket Reform keyboard firmware

This document is intended to help you get started hacking on the
MNT Pocket Reform keyboard firmware.  It's a small, tight system on a
well-documented base platform, so happy hacking!

## License

This document is licensed under the terms of the Creative Commons CC
BY-SA license.  The license of the surrounding firmware, other
portions of the Pocket Reform project, and the dependencies of this
project may be different, and this license applies only to this
documentation.

## Disclaimer

This early version of this document is authored by Ethan Blanton, who
was not the original author of this code.  There may be errors and
misunderstandings, which should be considered Ethan's fault and no
fault of the software authors.  Furthermore, the software may have
drifted away from this document in the meantime.  Feedback is welcome!

## Prerequisites

You will need an appropriate gcc compiler for ARM Cortex-M
development, CMake, Python, and gcab, as well as an appropriate
version of the Pico SDK.  The script `install-fw-dependencies.sh` will
install and configure these dependencies on a Debian system, including
self-hosted on the Pocket Reform itself.

It is advisable to have a convenient USB C keyboard, as well as a USB
PD power supply to power the Pocket Reform.  Mistakes in flashing or
bugs in any changes you make to the keyboard firmware may make it
difficult to recover without an external keyboard, and if your Pocket
Reform powers off while the keyboard is non-functional it is somewhat
arduous to put it back into a mode where it powers up immediately when
power is applied.

## Overview

The Pocket Reform keyboard consists of a Raspberry Pi RP2040
microcontroller, a passive keyboard matrix (including the buttons for
the trackball, via GPIO), and a string of WS2812-protocol RGB LEDs
(PIO driver for GPIO).  It is connected via cables to the trackball
sensor (I2C), OLED mini-display (I2C), and motherboard (via both USB
and UART).  A hardware watchdog timer monitors its execution and
reboots the firmware after one second without a watchdog update.

During initialization, USB interactions are scheduled on a 5 ms period
using a hardware timer provided by the Pico SDK.  The UART dedicated
to communicating with the system controller on the motherboard is
configured to process incoming data on the UART interrupt, and other
I/O is accomplished via polling.

After hardware initialization and configuration of timer handlers and
interrupt service routines, the firmware enters an infinite loop that
cycles through the polling I/O functions and handles second-level
interrupt processing on a periodic cycle no shorter than roughly 10 ms.

The trackball sensor and OLED mini-display are both attached to the
same I2C bus, and are accessed via blocking I/O in the main loop.
Each iteration of the loop, the trackball position is updated, the
menu is repainted if necessary, and the keyboard LEDs are adjusted if
necessary.

## Building

A build script is provided, `build.sh`, which runs CMake and then
builds the firmware.  It is a simple two-line affair, as CMake takes
care of most of the heavy lifting for build configuration.  After
successful build, you will find the file `build/pocket-hid.uf2`, which
is the file required by the firmware update for flashing.

## Flashing

**Note:** It is advisable to have a spare USB keyboard and USB C cable
available when updating the keyboard on a Pocket Reform in situ.
While the likelihood that your new firmware makes the keyboard
unusable depends on the changes you've made, it is relatively easy to
get the keyboard into an unfortunate configuration simply by
performing the update steps in the wrong order – a configuration
easily fixed by using an external keyboard to flash a known-good
firmware.

The provided script `flash.sh` will upload `build/pocket-hid.uf2` to
the built-in keyboard when run from a Pocket Reform.  It will also
upload to a Pocket Reform keyboard attached to another computer (which
requires a special cable).

To put the keyboard in a Pocket Reform into firmware update mode,
press Hyper+Enter and then the 'x' key.  The mini OLED display will
say "Entered firmware update mode," and the keyboard will wait for a
firmware upload.  While it is waiting, it does not work as a keyboard.
The `picotool` command in `flash.sh` will then provide a usable
firmware to the keyboard.

Keyboard flashing requires root privileges, so `flash.sh` uses `sudo`.
Make sure that you have used sudo before invoking `flash.sh`, or have
a second keyboard ready to enter your password while the keyboard
waits to be flashed!

A reasonable workflow is something like:

1. Run the command `sudo sleep 5 && ./flash.sh`
2. Enter your sudo password (if necessary) to begin the sleep
3. Press Hyper+Enter and then x on the keyboard
4. Wait
5. When the keyboard reboots (it will print "Reset by watchdog") on
   the OLED display), press and hold Hyper+Enter to power it back up

Using `sudo` for sleep isn't necessary at all, it just ensures that
the password is primed before pressing Hyper+Enter x and rendering the
keyboard unusable until the flash completes.

## The Keyboard Matrix

The keyboard matrix is wired to GPIO pins on the RP2040, defined in
`pins.h`.  The keyboard has six rows (the sixth is the trackball
buttons) and twelve columns, with a total of 64 keys.  Keypresses are
read by raising each column pin one at a time, then sensing each row.

## The onboard devices via I2C

The trackball sensor and OLED mini-display are both attached via the
same I2C port on the RP2040.  This means that sensing the trackball
and updating the OLED must be performed in sequence, synchronously, to
avoid contention in attempting to access the I2C bus.

## The LED backlight string

The entire LED backlight string is serially attached to a single GPIO
port on the RP2040.  The code to manage them is in `leds.c`, and it
uses a RP2040 PIO driver to clock out RGB values in a synchronous
fashion.

## USB

USB interactions are handled by tinyusb.  The Pocket Reform keyboard
presents a HID boot protocol keyboard on its USB interface; the
descriptors for this (both mouse and keyboard) are in
`usb_descriptors.c`.

Every five milliseconds, on an alarm interrupt, the function
`hid_task()` will be invoked.  This function first polls the keyboard
(including trackball keys) for keypresses and records the result.  It
then polls the USB bus for activity via tinyusb, and triggers the
sending of a USB HID report for the keyboard.  When this report
completes, the next polling of the USB device will trigger a USB HID
report for the trackball.

## sysctl UART

UART 1 on the RP2040 is cabled to the Pocket Reform system controller,
and used to control power management and retrieve status information
from the motherboard.  It communicates at 115200 baud, 8 bits, no
parity, 1 stop bit.  The protocol is defined by the system controller
firmware.

The communication interface for these interactions is in `remote.c`.
Incoming data from the system controller triggers an interrupt, which
fires an interrupt handler that copies the available data from the
UART into a static receive buffer.

Commands and requests for information are sent from the keyboard to
the system controller synchronously; the keyboard firmware sends a
command and then, if a response is expected, goes into a relatively
slow waiting loop until the receive interrupt handler indicates that a
complete response has been received.  At that point, the keyboard
firmware processes the completed reply and takes whatever action is
indicated.

## Recovering from mistakes

If you find that you cannot communicate with the keyboard, or it does
not seem to be working properly, there are several things to try.

First, there are two buttons on the keyboard PCB, PROG and RESET,
mounted just underneath the OLED display in the Pocket Reform case.
You can try pressing the reset button to see if the keyboard comes
back to life; you may need to press and hold Hyper+Enter to power it
back up.

Second, you can try cycling the power to the entire Pocket Reform, by
switching the battery disable switch accessible through the slot on
the left side of the monitor off and back on again.

Third, you can try putting it manually into bootloader mode, by
pressing and holding the PROG button and then pressing and releasing
the RESET button, then releasing PROG.  This should put the keyboard
into a state where you can use `flash.sh` to upload a new firmware.

## Resources

* [MNT Pocket Reform Operator
  Handbook](https://mntre.com/documentation/pocket-reform-handbook/)  
  This document has a lot of information about the architecture and
  usage of the Pocket Reform that is helpful to the firmware hacker.
* [MNT Pocket Reform DIY Assembly
  Manual](https://mntre.com/documentation/pocket-reform-diy-assembly-manual.pdf)  
  This document has detailed photos of the internal parts of the
  Pocket Reform, references for how and where cables are mounted ,etc.
  Even if you bought a pre-assembled Pocket Reform, there's good
  information to be found.
* [Raspberry Pi Pico SDK
  Documentation](https://www.raspberrypi.com/documentation/pico-sdk/)  
  In particular, the hardware APIs document describes usage of the
  GPIO, I2C, and UART ports.  The high level APIs documentation covers
  the timer used for HID updates.
* [RP2040
  Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf)  
  Most firmware hacking shouldn't require this document (the Pico SDK
  documentation tells you what you need to know if you stick to the
  SDK), but for some questions you just have to go to the source.
* [TinyUSB
  documentation](https://docs.tinyusb.org/en/latest/index.html)  
  Unfortunately, there's not much there.  The TinyUSB source includes
  a variety of example applications that are just about all there is
  in terms of documentation; even the source is only lightly
  commented.
* TODO: Add links for WS2812, OLED module, and trackball sensor
