#include "altmode.h"
#include "sysctl.h"
#include <stdio.h>

bool tmuxhs4446_write(uint8_t val) {
  sysctl_disable_irqs();
  uint8_t buf[2] = {2, val};
  int res = i2c_write_timeout_us(i2c0, TMUX_ADDR, buf, 2, false, I2C_TIMEOUT);
  sysctl_enable_irqs();
  return 2 == res;
}

int tmuxhs4446_read(uint8_t* buf) {
  sysctl_disable_irqs();
  uint8_t addr = 2;
  i2c_write_blocking(i2c0, TMUX_ADDR, &addr, 1, true);
  int res = i2c_read_timeout_us(i2c0, TMUX_ADDR, buf, 1, false, I2C_TIMEOUT);
  sysctl_enable_irqs();
  return res;
}

bool altmode_enable_dp(int flipped) {
  uint8_t conf2 = 1;
  uint8_t conf1 = 1;
  uint8_t conf0 = flipped?1:0;

  //printf("# [altmode] enable_dp, flipped: %d\n", flipped);
  bool res = tmuxhs4446_write((conf2<<2)|(conf1<<1)|conf0);
  //printf("# [altmode] enable_dp, result: %d\n", res);

  return res;
}

bool altmode_set(uint8_t conf) {
  printf("# [altmode] altmode_set, conf: %d\n", conf);
  bool res = tmuxhs4446_write(conf);
  printf("# [altmode] altmode_set, result: %d\n", res);

  return res;
}
