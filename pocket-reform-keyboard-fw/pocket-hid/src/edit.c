/*
  SPDX-License-Identifier: GPL-3.0-or-later
  MNT Pocket Reform Keyboard/Trackball Controller Firmware for RP2040
  Copyright 2021-2026 MNT Research GmbH (mntre.com)
 */

#include "edit.h"
#include "oled.h"
#include "usb_hid_keys.h"
#include "keyboard.h"
#include "pico/stdlib.h"
#include "remote.h"
#include <malloc.h>
#include <stdint.h>
#include <string.h>

#define EDIT_STATE_NORMAL 0
#define EDIT_STATE_ESC 1
#define EDIT_STATE_CSI 2
#define EDIT_STATE_WAIT_KEYUP -1
#define EDIT_LINE_MAX 20
#define CHR_BACKSP 0x8
#define CHR_NEWLINE 0xa
#define CHR_DOWN 0xb
#define CHR_UP 0xc
#define CHR_RETURN 0xd
#define CHR_RIGHT 0xe
#define CHR_LEFT 0xf
static int edit_state = EDIT_STATE_WAIT_KEYUP;
static char edit_line[EDIT_LINE_MAX];
static int edit_line_cursor = 0;
static uint8_t edit_y = 0;

// 0x00 - 0x52
#define KEY_TO_CHR_LAST 0x52
static uint8_t *key_to_chr =
  (uint8_t *)"....abcdefghijklmnopqrstuvwxyz1234567890"
             "\x0a" /* return */
             "\x1b" /* escape */
             "\x08" /* backspace */
             "\t -=[]\\<;'`,./"
             "."            /* caps lock */
             "............" /* f keys */
             "......"       /* prtscr etc incl pageup*/
             "\x7f"         /* del */
             ".."           /* end, pagedn */
             "\x0e"         /* shift out, we use as cursor left */
             "\x0f"         /* shift in, we use as cursor right */
             "\x0b"         /* vert tab, we use as cursor down */
             "\x0c"         /* form feed, we use as cursor up */
;
static uint8_t *key_to_chr_shifted =
  (uint8_t *)"....ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()"
             "\x0a" /* return */
             "\x1b" /* escape */
             "\x08" /* backspace */
             "\t _+{}|>:\"~<>?"
             "."            /* caps lock */
             "............" /* f keys */
             "......"       /* prtscr etc incl pageup*/
             "\x7f"         /* del */
             ".."           /* end, pagedn */
             "\x0e"         /* shift out, we use as cursor left */
             "\x0f"         /* shift in, we use as cursor right */
             "\x0b"         /* vert tab, we use as cursor down */
             "\x0c"         /* form feed, we use as cursor up */
;

static inline uint32_t board_millis(void) {
  return to_ms_since_boot(get_absolute_time());
}

void edit_render() {
  int x = 0;
  for (; x < EDIT_LINE_MAX; x++) {
    uint8_t chr = edit_line[x];
    gfx_poke(x, edit_y, chr);
    if (chr < 32) break;
  }
  for (; x < EDIT_LINE_MAX; x++) {
    gfx_poke(x, edit_y, ' ');
  }
  gfx_clear_invert();
  gfx_invert_char(edit_line_cursor, edit_y);
  gfx_flush();
}

void edit_line_del() {
  for (int i = edit_line_cursor; i < EDIT_LINE_MAX-1; i++) {
    char c = edit_line[i];
    char next = edit_line[i + 1];
    if (c > 31 && next > 31) {
      edit_line[i] = next;
    } else {
      edit_line[i] = 0;
      break;
    }
  }
}

static uint8_t prev_key = 0;
static uint32_t prev_key_ms = 0;
#define EDIT_REPEAT_DELAY_MS 500
#define EDIT_REPEAT_RATE_MS 50
static uint8_t repeat_state = 0;

void edit_insert_chr(uint8_t c) {
  if (edit_line_cursor < EDIT_LINE_MAX-1) {
    if (edit_line[EDIT_LINE_MAX-2] == 0) {
      for (int i = EDIT_LINE_MAX - 2; i >= edit_line_cursor; i--) {
	edit_line[i + 1] = edit_line[i];
      }
      edit_line[edit_line_cursor] = c;
      if (edit_line_cursor<EDIT_LINE_MAX) edit_line_cursor++;
    } else {
      // end of line
      edit_line[edit_line_cursor] = c;
      edit_line[edit_line_cursor+1] = 0;
    }
  }
}

void edit_input_key(uint8_t inkey, uint8_t shift) {
  if (edit_state == EDIT_STATE_WAIT_KEYUP) {
    // wait for initial key up event (coming from menu)
    if (inkey == 0) {
      edit_state = EDIT_STATE_NORMAL;
    }
    return;
  }

  uint32_t now_ms = board_millis();
  if (prev_key == inkey) {
    uint32_t passed_ms = now_ms - prev_key_ms;
    if (repeat_state == 0) {
      if (passed_ms < EDIT_REPEAT_DELAY_MS) {
	// too fast
	return;
      }
      repeat_state = 1;
    } else {
      if (passed_ms < EDIT_REPEAT_RATE_MS) {
	return;
      }
    }
  } else {
    repeat_state = 0;
  }
  prev_key = inkey;
  prev_key_ms = now_ms;

  // the purpose of this is to detect key-up events
  if (inkey == 0) return;
  
  if (inkey <= KEY_TO_CHR_LAST) {
    int inkey_c = 0;
    if (shift) {
      inkey_c = key_to_chr_shifted[inkey];
    } else {
      inkey_c = key_to_chr[inkey];
    }
    if (inkey_c == CHR_LEFT) {
      if (edit_line_cursor>0) edit_line_cursor--;
    } else if (inkey_c == CHR_RIGHT) {
      if (edit_line_cursor < (EDIT_LINE_MAX-1) &&
	  edit_line[edit_line_cursor] != 0) edit_line_cursor++;
    } else if (inkey_c == CHR_BACKSP) {
      if (edit_line_cursor > 0) {
	edit_line_cursor--;
      }
      edit_line_del();
    } else if (inkey_c == CHR_NEWLINE) {
      gfx_clear();
      gfx_flush();
      remote_try_command(edit_line, 1);
      edit_render();
    } else if (inkey_c >= 32) {
      edit_insert_chr(inkey_c);
    }
  }
  edit_render();
}

void edit_setup() {
  uint32_t now_ms = board_millis();
  prev_key = 0;
  prev_key_ms = now_ms;
  edit_state = EDIT_STATE_WAIT_KEYUP;
  repeat_state = 0;
  edit_line_cursor = 0;
  edit_y = 3;
  memset(edit_line, 0, EDIT_LINE_MAX);
  gfx_clear();
  gfx_flush();
  edit_render();
}
