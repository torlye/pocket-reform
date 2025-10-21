/*
  SPDX-License-Identifier: GPL-3.0-or-later
  MNT Pocket Reform Keyboard/Trackball Controller Firmware for RP2040
  Copyright 2021-2025 MNT Research GmbH (mntre.com)
*/

#ifndef _KEYBOARD_H_
#define _KEYBOARD_H_

#define KBD_VARIANT_QWERTY_US
#define KBD_COLS 12
#define KBD_ROWS 6
#define KBD_MATRIX_SZ KBD_COLS * KBD_ROWS + 4
#define KBD_DEFAULT_BACKLIGHT_COLOR 0x200020

#define TRACKBALL_FACTOR 2

void reset_keyboard_state(void);

#endif
