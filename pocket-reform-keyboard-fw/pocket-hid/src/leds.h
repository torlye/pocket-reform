#ifndef _LEDS_H
#define _LEDS_H

void led_init();
void led_turn_on();
void led_turn_off();
void led_set_rgb(uint32_t rgb);
void led_set_hsv();
void led_task(uint32_t rgb);
void led_mod_hue(int d);
void led_mod_brightness(int d);
void led_set_brightness(int b);
int led_get_brightness();
void led_mod_saturation(int d);
void led_set_saturation(int b);
void led_set_hue(int b);
void led_cycle_hue();
void led_bitmap(uint8_t row, const uint8_t* row_rgb);

#endif
