# MNT Pocket Reform: Swapped Ctrl and Hyper keys
This fork modifies `pocket-reform-keyboard-fw` to swap the position of the **Left Ctrl** and **Hyper** keys on the keyboard.

My muscle memory requires me to have the Ctrl key at the bottom left corner of the keyboard!

Nothing else is modified at the moment.

## First-time setup
Run `./install-fw-dependencies.sh`

## Building and flashing
1. cd to `pocket-reform-keyboard-fw/pocket-hid`
2. Run `./build.sh` to build
3. Run the command `sudo sleep 5 && ./flash.sh`
4. Enter your sudo password (if necessary) to begin the sleep
5. Press Hyper+Enter and then x on the keyboard to enter firmware upload mode
6. Wait for flashing to complete and for the controller to reset

---

Original README follows:

---

# MNT Pocket Reform (Research Project)

This repository contains specifications, schematics, PCB and case designs, and firmware source code for the MNT Pocket Reform laptop.

## License

Copyright 2021 MNT Research GmbH.

The following licenses are used in the project, unless specified differently in a particular subfolder:

- Schematics, PCBs: [CERN-OHL-S v2](https://www.ohwr.org/project/cernohl/wikis/uploads/002d0b7d5066e6b3829168730237bddb/cern_ohl_s_v2.txt)
- Other documentation, artwork, photos and drawings that are not trademarks (see below): [CC-BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/legalcode)
- Software, firmware: Various. [GPL 3.0](https://www.gnu.org/licenses/gpl-3.0.en.html) if not specified otherwise in the file/subdirectory.

The "MNT" and "MNT REFORM" logos are trademarks of MNT Research GmbH. You may not use these in derived works. The reason for this is that we cannot be responsible for regulatory issues with derived boards and we cannot support them. If someone sees an MNT brand on a product, it has to be clear that it comes from MNT Research and not from a third party.
