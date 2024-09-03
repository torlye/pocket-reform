#ifndef _POCKET_MP2650_H
#define _POCKET_MP2650_H

#include <stdint.h>
#include "sysctl.h"

uint8_t mps_read_byte(uint8_t addr);

uint16_t mps_read_word(uint8_t addr);

float mps_word_to_ntc(uint16_t w);

float mps_word_to_3200(uint16_t w);
float mps_word_to_6400(uint16_t w);

float mps_word_to_12800(uint16_t w);

float mps_word_to_w(uint16_t w);

float mps_word_to_temp(uint16_t w);

void mps_read_buf(uint8_t addr, uint8_t size, uint8_t *buf);

void mps_write_byte(uint8_t addr, uint8_t byte);

void mps_write_buf(uint8_t addr, uint8_t size, const uint8_t *buf);

#endif