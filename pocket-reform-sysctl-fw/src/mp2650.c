#include "mp2650.h"

uint8_t mps_read_byte(uint8_t addr)
{
  uint8_t buf;
  i2c_write_blocking(i2c0, MPS_ADDR, &addr, 1, true);
  i2c_read_blocking(i2c0, MPS_ADDR, &buf, 1, false);
  return buf;
}

uint16_t mps_read_word(uint8_t addr)
{
  uint8_t buf[2];
  i2c_write_blocking(i2c0, MPS_ADDR, &addr, 1, true);
  i2c_read_blocking(i2c0, MPS_ADDR, buf, 2, false);
  uint16_t result = ((uint16_t)buf[1]<<8) | (uint16_t)buf[0];
  return result;
}

float mps_word_to_ntc(uint16_t w)
{
  float result = (float)(w&0xfff)*1.6/4096.0;
  return result;
}

float mps_word_to_3200(uint16_t w)
{
  float result = (w>>10)*100
    + ((w&(1<<9))>>9)*50
    + ((w&(1<<8))>>8)*25
    + ((w&(1<<7))>>7)*12.5
    + ((w&(1<<6))>>6)*6.25;
  return result;
}

float mps_word_to_6400(uint16_t w)
{
  float result = (w>>9)*100
    + ((w&(1<<8))>>8)*50
    + ((w&(1<<7))>>7)*25
    + ((w&(1<<6))>>6)*12.5;
  return result;
}

float mps_word_to_12800(uint16_t w)
{
  float result = (w>>8)*100
    + ((w&(1<<7))>>7)*50
    + ((w&(1<<6))>>6)*25;
  return result;
}

float mps_word_to_w(uint16_t w)
{
  float result = (w>>9)
    + ((w&(1<<8))>>8)*0.5
    + ((w&(1<<7))>>7)*0.25
    + ((w&(1<<6))>>6)*0.125;
  return result;
}

float mps_word_to_temp(uint16_t w)
{
  // tj=903-2.578*t
  // tj-903=-2.578*t
  // (tj-903)/-2.578=t

  float result = (float)(w>>6);
  result = (result - 903) / -2.578;
  //result = 903-2.578*result;
  return result;
}

void mps_read_buf(uint8_t addr, uint8_t size, uint8_t *buf)
{
  i2c_write_blocking(i2c0, MPS_ADDR, &addr, 1, true);
  i2c_read_blocking(i2c0, MPS_ADDR, buf, size, false);
}

void mps_write_byte(uint8_t addr, uint8_t byte)
{
  uint8_t buf[2] = {addr, byte};
  i2c_write_blocking(i2c0, MPS_ADDR, buf, 2, false);
}

void mps_write_buf(uint8_t addr, uint8_t size, const uint8_t *buf)
{
  uint8_t txbuf[size + 1];
  txbuf[0] = addr;
  for (int i = 0; i < size; i++) {
    txbuf[i + 1] = buf[i];
  }
  i2c_write_blocking(i2c0, MPS_ADDR, txbuf, size + 1, false);
}
