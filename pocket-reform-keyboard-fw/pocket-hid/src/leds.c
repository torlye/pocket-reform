#include <stdint.h>
#include "leds.h"
#include "ws2812.pio.h"
#include "keyboard.h"
#include "pins.h"

#define BRIGHTNESS_MAX 0x96

static int led_brightness = 0;
static int led_saturation = 255;
static int led_hue = 127;

typedef struct rgb_color
{
  uint8_t r;
  uint8_t g;
  uint8_t b;
} rgb_color;

typedef struct hsv_color
{
  uint8_t h;
  uint8_t s;
  uint8_t v;
} hsv_color;

uint8_t led_rgb_buf[KBD_COLS*KBD_ROWS*3];

void led_init() {
  // PIO for RGB LEDs
  PIO pio = pio0;
  uint sm = 0;
  uint offset = pio_add_program(pio, &ws2812_program);

  ws2812_program_init(pio, sm, offset, PIN_LEDS, 800000, false);
}

void led_task(uint32_t color) {
  uint8_t b = color & 0xff;
  uint8_t g = (color >> 8) & 0xff;
  uint8_t r = (color >> 16) & 0xff;
  uint32_t pixel_grb = g<<16 | r<<8 | b;

  for (int y=0; y<KBD_ROWS; y++) {
    int w = KBD_COLS;
    if (y==5) w = 4;
    for (int x=0; x<w; x++) {
      pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
    }
  }
}

void led_bitmap(uint8_t row, const uint8_t* row_rgb) {
  // row = 5 -> commit
  if (row > 5) return;

  uint8_t* store = &led_rgb_buf[row*3*KBD_COLS];
  int offset = 0;
  for (int x=0; x<3*KBD_COLS; x++) {
    if (row == 5 && x == 0*3) offset = 3*3;
    if (row == 5 && x == 2*3) offset = 7*3;
    store[x] = row_rgb[x+offset];
    if (row == 5 && x == 4*3) break;
  }

  if (row == 5) {
    for (int y=0; y<KBD_ROWS; y++) {
      int w = KBD_COLS;
      if (y==5) w = 4;
      uint8_t* bitmap = &led_rgb_buf[y*3*KBD_COLS];
      for (int x=0; x<w; x++) {
        uint32_t pixel_grb = (bitmap[1]<<16u) | (bitmap[2]<<8u) | bitmap[0];
        pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
        bitmap+=3;
      }
    }
  }
}

hsv_color rgb_to_hsv(rgb_color rgb)
{
  hsv_color hsv;
  uint8_t rgb_min, rgb_max;

  rgb_min = rgb.r < rgb.g ? (rgb.r < rgb.b ? rgb.r : rgb.b) : (rgb.g < rgb.b ? rgb.g : rgb.b);
  rgb_max = rgb.r > rgb.g ? (rgb.r > rgb.b ? rgb.r : rgb.b) : (rgb.g > rgb.b ? rgb.g : rgb.b);

  hsv.v = rgb_max;
  if (hsv.v == 0) {
    hsv.h = 0;
    hsv.s = 0;
    return hsv;
  }

  hsv.s = (uint8_t)(255 * (long)(rgb_max - rgb_min) / hsv.v);
  if (hsv.s == 0) {
    hsv.h = 0;
    return hsv;
  }

  if (rgb_max == rgb.r)
    hsv.h = (uint8_t)(0 + 43 * (rgb.g - rgb.b) / (rgb_max - rgb_min));
  else if (rgb_max == rgb.g)
    hsv.h = (uint8_t)(85 + 43 * (rgb.b - rgb.r) / (rgb_max - rgb_min));
  else
    hsv.h = (uint8_t)(171 + 43 * (rgb.r - rgb.g) / (rgb_max - rgb_min));

  return hsv;
}

rgb_color hsv_to_rgb(hsv_color hsv)
{
  rgb_color rgb;
  uint8_t region, remainder, p, q, t;

  if (hsv.s == 0) {
    rgb.r = hsv.v;
    rgb.g = hsv.v;
    rgb.b = hsv.v;
    return rgb;
  }

  region = hsv.h / 43;
  remainder = (uint8_t)((hsv.h - (region * 43)) * 6);

  p = (uint8_t)((hsv.v * (255 - hsv.s)) >> 8);
  q = (uint8_t)((hsv.v * (255 - ((hsv.s * remainder) >> 8))) >> 8);
  t = (uint8_t)((hsv.v * (255 - ((hsv.s * (255 - remainder)) >> 8))) >> 8);

  switch (region) {
  case 0:
    rgb.r = hsv.v; rgb.g = t; rgb.b = p;
    break;
  case 1:
    rgb.r = q; rgb.g = hsv.v; rgb.b = p;
    break;
  case 2:
    rgb.r = p; rgb.g = hsv.v; rgb.b = t;
    break;
  case 3:
    rgb.r = p; rgb.g = q; rgb.b = hsv.v;
    break;
  case 4:
    rgb.r = t; rgb.g = p; rgb.b = hsv.v;
    break;
  default:
    rgb.r = hsv.v; rgb.g = p; rgb.b = q;
    break;
  }

  return rgb;
}

// turn off the LEDs while not overwriting the last setting
void led_turn_off() {
  led_task(0);
}

// restore the last setting
void led_turn_on() {
  led_set_hsv();
}

void led_set_rgb(uint32_t rgb) {
  hsv_color hsv;
  rgb_color rgb_;
  rgb_.r = (rgb >> 16) & 0xff;
  rgb_.g = (rgb >> 8) & 0xff;
  rgb_.b = (rgb) & 0xff;
  hsv = rgb_to_hsv(rgb_);
  led_hue = hsv.h;
  led_saturation = hsv.s;
  led_brightness = hsv.v;

  led_task(rgb);
}

void led_set_hsv() {
  hsv_color hsv;
  rgb_color rgb;
  hsv.h = (uint8_t)led_hue;
  hsv.s = (uint8_t)led_saturation;
  hsv.v = (uint8_t)led_brightness;

  rgb = hsv_to_rgb(hsv);
  uint32_t rgb32 = (rgb.r<<16)|(rgb.g<<8)|(rgb.b);
  led_task(rgb32);
}

void led_mod_brightness(int d) {
  led_brightness+=(d/2);
  if (led_brightness>BRIGHTNESS_MAX) led_brightness = BRIGHTNESS_MAX;
  if (led_brightness<0) led_brightness = 0;
  led_set_hsv();
}

void led_mod_hue(int d) {
  led_hue+=d;
  if (led_hue>0xff) led_hue = 0;
  if (led_hue<0) led_hue = 0xff;
  led_set_hsv();
}

void led_mod_saturation(int d) {
  led_saturation+=d;
  if (led_saturation>0xff) led_saturation = 0xff;
  if (led_saturation<0) led_saturation = 0;
  led_set_hsv();
}

void led_set_saturation(int s) {
  led_saturation = s;
  led_set_hsv();
}

void led_set_brightness(int b) {
  led_brightness = b;
  if (led_brightness>BRIGHTNESS_MAX) led_brightness = BRIGHTNESS_MAX;
  if (led_brightness<0) led_brightness = 0;
  led_set_hsv();
}

int led_get_brightness() {
  return led_brightness;
}

void led_cycle_hue() {
  led_hue++;
  if (led_hue>0xff) led_hue = 0;
  led_set_hsv();
}

void led_set_hue(int h) {
  led_hue = h;
  led_set_hsv();
}
