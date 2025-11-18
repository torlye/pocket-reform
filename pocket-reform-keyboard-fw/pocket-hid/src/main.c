/*
  SPDX-License-Identifier: GPL-3.0-or-later
  MNT Pocket Reform Keyboard/Trackball Controller Firmware for RP2040
  Copyright 2021-2025 MNT Research GmbH (mntre.com)

  TinyUSB callbacks/code based on code
  Copyright 2019 Ha Thach (tinyusb.org)
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "tusb.h"

#include "pico/time.h"
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"

#include "usb_descriptors.h"
#include "oled.h"
#include "leds.h"
#include "menu.h"
#include "remote.h"
#include "matrix.h"
#include "keyboard.h"
#include "pins.h"

#include "ws2812.pio.h"

#define ADDR_SENSOR (0x79)

#define MAX_SCANCODES 6
static uint8_t pressed_scancodes[MAX_SCANCODES] = {0,0,0,0,0,0};
static int pressed_keys = 0;
static volatile uint32_t led_value = 0;

static bool hid_task(struct repeating_timer *t);
static int process_keyboard(uint8_t* resulting_scancodes);
static int poll_trackball(void);

static void service_menu(void);
static void enter_menu_mode(void);
static void exit_menu_mode(void);

#define UART_ID uart1
#define BAUD_RATE 115200
#define DATA_BITS 8
#define STOP_BITS 1
#define PARITY    UART_PARITY_NONE

// can be used as a global clock, incrementing around every ~5ms
static int hid_task_counter = 0;
static int trackball_motion = 0;
static bool hyper_key = 0; // holding HYPER?
static bool shift_key = 0; // holding SHIFT?
static double tb_nx = 0;
static double tb_ny = 0;

// This state machine describes the global interaction state of the
// OLED-displayed menu.
enum MenuState {
  MENU_STATE_INACTIVE, // not displayed
  MENU_STATE_ACTIVE,   // displayed and the user is interacting
  MENU_STATE_ENTER,    // the user has requested the menu be displayed
  MENU_STATE_EXIT      // the menu interaction is over
};
static enum MenuState menu_state = MENU_STATE_INACTIVE;

// The next menu item (by key code) to invoke
static int request_menu_function = 0;
// The last key pressed while the menu was active
static uint8_t last_menu_key = 0;

static inline uint32_t board_millis(void) {
  return to_ms_since_boot(get_absolute_time());
}

int main(void)
{
  set_sys_clock_48mhz();

  tusb_init();

  uart_init(UART_ID, BAUD_RATE);
  uart_set_format(UART_ID, DATA_BITS, STOP_BITS, PARITY);
  uart_set_hw_flow(UART_ID, false, false);
  uart_set_fifo_enabled(UART_ID, true);
  gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
  gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);
  unsigned int UART_IRQ = UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;

  gpio_init(PIN_LEDS);
  gpio_set_dir(PIN_LEDS, true); // output

  /* Configure columns to output, bring low */
  gpio_init_mask(PIN_COL_MASK);
  gpio_set_dir_out_masked(PIN_COL_MASK);
  gpio_put_masked(PIN_COL_MASK, 0);

  /* Configure rows as input and enable pull-downs */
  gpio_init_mask(PIN_ROW_MASK);
  gpio_pull_down(PIN_ROW1);
  gpio_pull_down(PIN_ROW2);
  gpio_pull_down(PIN_ROW3);
  gpio_pull_down(PIN_ROW4);
  gpio_pull_down(PIN_ROW5);
  gpio_pull_down(PIN_ROW6);

  i2c_init(i2c0, 400 * 1000);
  gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
  gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);

  bi_decl(bi_2pins_with_func(PIN_SDA, PIN_SCL, GPIO_FUNC_I2C));

  unsigned char buf[] = {0x7f, 0x00, 0x00, 0x00};
  i2c_write_blocking(i2c0, ADDR_SENSOR, buf, 2, false);

  buf[0] = 0x05;
  buf[1] = 0x01;
  i2c_write_blocking(i2c0, ADDR_SENSOR, buf, 2, false);

  led_init();
  led_turn_off();

  gfx_init();

  // watchdog crash recovery
  if (watchdog_caused_reboot()) {
    gfx_clear();
    gfx_on();
    gfx_poke_str(1, 1, "Reset by watchdog.");
    gfx_flush();
  }

  // reset if main loop is stuck for 1000ms
  watchdog_enable(1000, 1);

  // UART IRQ
  irq_set_exclusive_handler(UART_IRQ, remote_on_uart_rx);
  uart_set_hw_flow(UART_ID, false, false);
  // bool rx_has_data, bool tx_needs_data
  uart_set_irq_enables(UART_ID, true, false);
  uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
  uart_set_fifo_enabled(UART_ID, true);
  irq_set_enabled(UART_IRQ, true);

  // call USB task every 5ms
  struct repeating_timer timer;
  add_repeating_timer_ms(-5, hid_task, NULL, &timer);

  // try to determine system controller power state
  for (int i=0; i<2; i++) {
    remote_get_voltages(1);
  }
  if (remote_get_power_state()) {
    // initial backlight color
    led_set_rgb(KBD_DEFAULT_BACKLIGHT_COLOR);
  }

  unsigned int cycles = 0;
  while (1) {
    // the sleep time directly influences
    // trackball tracking speed
    sleep_ms(10);
    watchdog_update();

    // we can't do this in parallel
    // with OLED updating because they're
    // both on the same I2C port
    trackball_motion = poll_trackball();

    // service menu requests
    service_menu();

    // notifications / page refreshing
    cycles++;
    if (cycles%1000 == 0) {
      // quietly get voltages to update power state
      //remote_get_voltages(1);
    }
    if (cycles%50 == 0) {
      refresh_menu_page();

      // if device is off and user is pressing random keys,
      // show a hint for turning on the device
      if (!remote_get_power_state()) {
        if (pressed_keys>0 && menu_state != MENU_STATE_ACTIVE && !hyper_key && !last_menu_key) {
          execute_menu_function(KEY_H);
        }
      }
    }

    // backlight hue/value wheel
    if (trackball_motion && hyper_key) {
      if (tb_ny) {
        // shift held? saturation
        if (shift_key) {
          led_mod_saturation(tb_ny);
        } else {
          led_mod_brightness(tb_ny);
        }
      }
      if (tb_nx) {
        led_mod_hue(tb_nx);
      }
    }
  }

  return 0;
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void)
{
  // called
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
  // never called
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
  // never called
  (void) remote_wakeup_en;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
  // never called
}

// RGB LEDS

int __attribute__((optimize("Os"))) delay300ns() {
  // ~300ns
  asm volatile(
               "mov  r0, #9\n"    		// 1 cycle (was 10)
               "loop1: sub  r0, r0, #1\n"	// 1 cycle
               "bne   loop1\n"          	// 2 cycles if loop taken, 1 if not
               );
  return 0;
}

//--------------------------------------------------------------------+
// USB HID
//--------------------------------------------------------------------+

typedef struct TU_ATTR_PACKED
{
  uint8_t buttons; /**< buttons mask for currently pressed buttons in the mouse. */
  int8_t  x;       /**< Current delta x movement of the mouse. */
  int8_t  y;       /**< Current delta y movement on the mouse. */
  int8_t  wheel;   /**< Current delta wheel movement on the mouse. */
  int8_t  pan;     // using AC Pan
} hid_trackball_report_t;

bool tud_hid_trackball_report(uint8_t report_id,
                            uint8_t buttons, int8_t x, int8_t y, int8_t vertical, int8_t horizontal)
{
  hid_trackball_report_t report =
  {
    .buttons = buttons,
    .x       = x,
    .y       = y,
    .wheel   = vertical,
    .pan     = horizontal
  };

  return tud_hid_report(report_id, &report, sizeof(report));
}

static uint8_t matrix_debounce[KBD_COLS*KBD_ROWS];
static uint8_t matrix_state[KBD_COLS*KBD_ROWS];

static uint32_t hyper_enter_long_press_start_ms = 0;

static uint8_t* active_matrix = matrix;

static void service_menu() {
  // These items should be in this order, because the state
  // transitions in each clause may trigger a transition to the next
  // clause.
  if (menu_state == MENU_STATE_ENTER) {
    enter_menu_mode();
  }
  if (request_menu_function != 0) {
    if (!execute_menu_function(request_menu_function)) {
      menu_state = MENU_STATE_EXIT;
    }
    request_menu_function = 0;
  }
  if (menu_state == MENU_STATE_EXIT) {
    exit_menu_mode();
  }
}

// enter the menu
static void enter_menu_mode(void) {
  menu_state = MENU_STATE_ACTIVE;
  reset_and_render_menu();
}

static void exit_menu_mode(void) {
  menu_state = MENU_STATE_INACTIVE;
}

void reset_keyboard_state(void) {
  for (int i = 0; i < KBD_COLS*KBD_ROWS; i++) {
    matrix_debounce[i] = 0;
    matrix_state[i] = 0;
  }
  last_menu_key = 0;
  reset_menu();
}

// this is called in a timer interrupt, no sleep() functions
// allowed!
static int process_keyboard(uint8_t* resulting_scancodes) {
  // how many keys are pressed this round
  uint8_t total_pressed = 0;
  uint8_t used_key_codes = 0;

  for (int i=0; i<MAX_SCANCODES; i++) {
    pressed_scancodes[i] = 0;
  }

  for (int x = 0; x < KBD_COLS; x++) {
    uint32_t rows = 0;

    switch (x) {
    case 0: gpio_put(PIN_COL1, 1); break;
    case 1: gpio_put(PIN_COL2, 1); break;
    case 2: gpio_put(PIN_COL3, 1); break;
    case 3: gpio_put(PIN_COL4, 1); break;
    case 4: gpio_put(PIN_COL5, 1); break;
    case 5: gpio_put(PIN_COL6, 1); break;
    case 6: gpio_put(PIN_COL7, 1); break;
    case 7: gpio_put(PIN_COL8, 1); break;
    case 8: gpio_put(PIN_COL9, 1); break;
    case 9: gpio_put(PIN_COL10, 1); break;
    case 10: gpio_put(PIN_COL11, 1); break;
    case 11: gpio_put(PIN_COL12, 1); break;
    }

    // wait for signal to stabilize
    busy_wait_us(1);

    rows = gpio_get_all();

    // clear the column pin for idle/the next iteration
    gpio_put_masked(PIN_COL_MASK, 0);

    for (int y = 0; y < KBD_ROWS; y++) {
      uint8_t keycode;
      int loc = y*KBD_COLS+x;
      keycode = active_matrix[loc];
      uint8_t pressed = 0;
      uint8_t debounced_pressed = 0;

      switch (y) {
      case 0:  pressed = (rows & (1u<<PIN_ROW1)) != 0; break;
      case 1:  pressed = (rows & (1u<<PIN_ROW2)) != 0; break;
      case 2:  pressed = (rows & (1u<<PIN_ROW3)) != 0; break;
      case 3:  pressed = (rows & (1u<<PIN_ROW4)) != 0; break;
      case 4:  pressed = (rows & (1u<<PIN_ROW5)) != 0; break;
      case 5:  pressed = (rows & (1u<<PIN_ROW6)) != 0; break;
      }

      // shift new state as bit into debounce "register"
      matrix_debounce[loc] = (uint8_t)(matrix_debounce[loc]<<1)|pressed;

      // if unclear state, we need to keep the last state of the key
      if (matrix_debounce[loc] == 0x00) {
        matrix_state[loc] = 0;
      } else if (matrix_debounce[loc] == 0x01) {
        matrix_state[loc] = 1;
      }
      debounced_pressed = matrix_state[loc];

      if (debounced_pressed) {
        total_pressed++;

        // hyper + enter? open OLED menu
        if (keycode == KEY_ENTER && hyper_key) {
          if (!last_menu_key) {
            if (menu_state != MENU_STATE_ACTIVE) {
              menu_state = MENU_STATE_ENTER;
            }
            uint32_t now_ms = board_millis();
            if (!now_ms) now_ms++;

            if (!hyper_enter_long_press_start_ms) {
              hyper_enter_long_press_start_ms = now_ms;
              // edge case
            }
            if (now_ms - hyper_enter_long_press_start_ms > 1000) {
              // turn on computer after 2 seconds of holding hyper + enter
              request_menu_function = KEY_1;
              menu_state = MENU_STATE_EXIT;
              last_menu_key = KEY_1;
              hyper_enter_long_press_start_ms = 0;
            }
          }
        } else if (keycode == KEY_COMPOSE) {
          hyper_key = 1;
          active_matrix = matrix_fn;
        } else {
          if (menu_state == MENU_STATE_ACTIVE) {
            // not holding the same key?
            if (last_menu_key != keycode) {
              // hyper/menu functions
              request_menu_function = keycode;
              // don't repeat action while key is held down
              last_menu_key = keycode;
            }
          } else if (!last_menu_key) {
            // not menu mode, regular key: report keypress via USB
            // 6 keys is the limit in the HID descriptor
            if (used_key_codes < MAX_SCANCODES && resulting_scancodes && y < 5) {
              resulting_scancodes[used_key_codes++] = keycode;
            }
          }
        }

        if (keycode == KEY_LEFTSHIFT) {
          shift_key = 1;
        }
      } else {
        // key not pressed
        if (keycode == KEY_COMPOSE) {
          hyper_key = 0;
          active_matrix = matrix;
          hyper_enter_long_press_start_ms = 0;
        } else if (keycode == KEY_ENTER) {
          hyper_enter_long_press_start_ms = 0;
        }

        if (keycode == KEY_LEFTSHIFT) {
          shift_key = 0;
        }
      }
    }
  }

  // if no more keys are held down, allow a new menu command
  if (total_pressed < 1) {
    last_menu_key = 0;
  }

  return used_key_codes;
}

static int scroll_toggle = 0;

static int tb_btn_left = 0;
static int tb_btn_right = 0;
static int tb_btn_scroll = 0;
static int tb_btn_middle = 0;
// TODO: implement HID commands to update these
static int tb_btn_left_idx = KBD_COLS*5+4;
static int tb_btn_right_idx = KBD_COLS*5+8;
static int tb_btn_scroll_idx = KBD_COLS*5+7;
static int tb_btn_middle_idx = KBD_COLS*5+3;

// returns motion yes/no
static int poll_trackball()
{
  tb_btn_left = matrix_state[tb_btn_left_idx]>0;
  tb_btn_middle = matrix_state[tb_btn_middle_idx]>0;
  tb_btn_right = matrix_state[tb_btn_right_idx]>0;
  tb_btn_scroll = matrix_state[tb_btn_scroll_idx]>0;

  uint8_t buf[] = {0x7f, 0x00, 0x00, 0x00};

  buf[0] = 0x02;

  i2c_write_blocking_until(i2c0, ADDR_SENSOR, buf, 1, true, make_timeout_time_ms(2));
  i2c_read_blocking_until(i2c0, ADDR_SENSOR, buf, 1, false, make_timeout_time_ms(2));

  if (buf[0] & 0xf0) {
    buf[0] = 0x03;
    i2c_write_blocking_until(i2c0, ADDR_SENSOR, buf, 1, true, make_timeout_time_ms(2));
    i2c_read_blocking_until(i2c0, ADDR_SENSOR, buf, 2, false, make_timeout_time_ms(2));

    tb_nx = (double)((int8_t)buf[0]);
    tb_ny = (double)((int8_t)buf[1]);

    return 1;
  }
  return 0;
}

static void send_hid_report(uint8_t report_id)
{
  if (!tud_hid_ready()) {
    return;
  }

  switch (report_id) {
    case REPORT_ID_KEYBOARD:
    {
      tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, pressed_scancodes);
    }
    break;

    case REPORT_ID_MOUSE:
    {
      int buttons = tb_btn_left | (tb_btn_right<<1) | (tb_btn_middle<<2);

      if (trackball_motion) {
        // no button, right + down, no scroll pan
        if (tb_btn_scroll || scroll_toggle) {
          tud_hid_mouse_report(REPORT_ID_MOUSE, (uint8_t)buttons, 0, 0, TRACKBALL_FACTOR*tb_ny, -TRACKBALL_FACTOR*tb_nx);
        } else {
          tud_hid_mouse_report(REPORT_ID_MOUSE, (uint8_t)buttons, -TRACKBALL_FACTOR*tb_nx, -TRACKBALL_FACTOR*tb_ny, 0, 0);
        }
      } else {
        if (tb_btn_middle && tb_btn_scroll) {
          // enter sticky scroll mode
          scroll_toggle = 1;
        } else {
          if (tb_btn_middle && scroll_toggle) {
            // exit sticky scroll mode
            scroll_toggle = 0;
          } else {
            // actually report the buttons
            tud_hid_mouse_report(REPORT_ID_MOUSE, (uint8_t)buttons, 0, 0, 0, 0);
          }
        }
      }
    }
    break;

    case REPORT_ID_CONSUMER_CONTROL:
    {
      // use to avoid send multiple consecutive zero report
      /*static bool has_consumer_key = false;

      if ( btn )
      {
        // volume down
        uint16_t volume_down = HID_USAGE_CONSUMER_VOLUME_DECREMENT;
        tud_hid_report(REPORT_ID_CONSUMER_CONTROL, &volume_down, 2);
        has_consumer_key = true;
      }else
      {
        // send empty key report (release key) if previously has key pressed
        uint16_t empty_key = 0;
        if (has_consumer_key) tud_hid_report(REPORT_ID_CONSUMER_CONTROL, &empty_key, 2);
        has_consumer_key = false;
        }*/
    }
    break;

    case REPORT_ID_GAMEPAD:
    {
      // TODO: later
    }
    break;

    default: break;
  }
}

// Every 5ms, we will sent 1 report for each HID profile (keyboard, mouse etc ..)
// tud_hid_report_complete_cb() is used to send the next report after previous one is complete
static bool hid_task(__unused struct repeating_timer *t)
{
  tud_task();
  pressed_keys = process_keyboard(pressed_scancodes);

  send_hid_report(REPORT_ID_KEYBOARD);
  hid_task_counter++;

  // timer should continue calling us
  return true;
}

// Invoked when sent REPORT successfully to host
// Application can use this to send the next report
// Note: For composite reports, report[0] is report ID
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len)
{
  (void) instance;
  (void) len;

  uint8_t next_report_id = report[0] + 1;

  if (next_report_id < REPORT_ID_COUNT) {
    send_hid_report(next_report_id);
  }
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
  // TODO not Implemented
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) reqlen;

  return 0;
}

#define CMD_TEXT_FRAME      "OLED"     // fill the screen with a single wall of text
#define CMD_OLED_CLEAR      "WCLR"     // clear the oled display
#define CMD_OLED_BITMAP     "WBIT"     // (u16 offset, u8 bytes...) write raw bytes into the oled framebuffer
#define CMD_POWER_OFF       "PWR0"     // turn off power rails
#define CMD_RGB_BACKLIGHT   "LRGB"     // keyboard backlight rgb
#define CMD_RGB_BRT         "LBRT"     // keyboard backlight brightness
#define CMD_RGB_SAT         "LSAT"     // keyboard backlight saturation
#define CMD_RGB_HUE         "LHUE"     // keyboard backlight saturation
#define CMD_RGB_BITMAP      "XRGB"     // push rgb backlight bitmap
#define CMD_LOGO            "LOGO"     // play logo animation

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
  (void) instance;
  (void) buffer;

  if (bufsize < 5) return;

  if (report_type == HID_REPORT_TYPE_OUTPUT) {
    // TODO this should really be REPORT_ID_KEYBOARD instead of 'x'
    if (report_id == 'x') {
      const char* cmd = (const char*)buffer;

      // uncomment for debugging
      /*char repinfo[64];
      snprintf(repinfo, sizeof(repinfo)-1, "cm: %c%c%c%c 4: %d sz: %d", cmd[0],cmd[1],cmd[2],cmd[3], cmd[4], bufsize);
      gfx_poke_str(0, 0, repinfo);
      gfx_flush();*/

      if (cmd == strnstr(cmd, CMD_TEXT_FRAME, 4)) {
        // print up to 4 lines (with 21 chars each) of text
        gfx_clear();
        gfx_on();

        int c = 4;
        for (uint8_t y=0; y<4; y++) {
          for (uint8_t x=0; x<21; x++) {
            if (buffer[c] == '\n') {
              c++;
              x = 0;
              y++;
            } else if (x < 21 && y < 4) {
              gfx_poke(x, y, buffer[c++]);
            }
            if (c>=bufsize) break;
          }
        }
        gfx_flush();
      }
      else if (cmd == strnstr(cmd, CMD_POWER_OFF, 4)) {
        // power the computer off
        reset_menu();
        remote_turn_off_som();
        reset_keyboard_state();
      }
      else if (cmd == strnstr(cmd, CMD_OLED_CLEAR, 4)) {
        // clear the OLED display
        gfx_clear();
        gfx_flush();
      }
      else if (cmd == strnstr(cmd, CMD_OLED_BITMAP, 4)) {
        // render a monochrome (1-bit) bitmap to the OLED display
        matrix_render_direct(&buffer[4]);
      }
      else if (cmd == strnstr(cmd, CMD_RGB_BITMAP, 4)) {
        // set a row of keyboard LEDs at once as 12 "pixels"
        // row, data (12 * 3 rgb bytes)
        led_bitmap(buffer[4], &buffer[5]);
      }
      else if (cmd == strnstr(cmd, CMD_RGB_BACKLIGHT, 4)) {
        // set a uniform colored RGB backlight
        uint32_t pixel_rgb = (uint32_t)((buffer[6]<<16u) | (buffer[5]<<8u) | buffer[4]);
        led_set_rgb(pixel_rgb);
      }
      else if (cmd == strnstr(cmd, CMD_RGB_BRT, 4)) {
        // modify brightness component of RGB backlight
        int val = (int)buffer[4];
        led_set_brightness(val);
      }
      else if (cmd == strnstr(cmd, CMD_RGB_SAT, 4)) {
        // modify saturation component of RGB backlight
        int val = (int)buffer[4];
        led_set_saturation(val);
      }
      else if (cmd == strnstr(cmd, CMD_RGB_HUE, 4)) {
        // modify hue component of RGB backlight
        int val = (int)buffer[4];
        led_set_hue(val);
      }
      else if (cmd == strnstr(cmd, CMD_LOGO, 4)) {
        anim_hello();
      }
    }
  }
}

// TODO
// from https://github.com/raspberrypi/pico-sdk/issues/1118
// Support for default BOOTSEL reset by changing baud rate
/*void tud_cdc_line_coding_cb(__unused uint8_t itf, cdc_line_coding_t const* p_line_coding) {
    if (p_line_coding->bit_rate == 110) {
        reset_usb_boot(gpio_mask, 0);
    }
}*/

void mntre_reset_callback(void) {
    // nothing to do
}
