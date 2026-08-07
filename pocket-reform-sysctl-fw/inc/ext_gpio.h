#ifndef _EXT_GPIO_H
#define _EXT_GPIO_H

#include <stdint.h>
#include <stdbool.h>

#define GPIO_EXT_DISP_1EN_2BL 0
#define GPIO_EXT_5V_EN 1
#define GPIO_EXT_3V3_EN 2
#define GPIO_EXT_HUB_PWR_EN 3
#define GPIO_EXT_DISP_RESET_N 4
#define GPIO_EXT_DISP_BL_PWR_EN 5
#define GPIO_EXT_PCIE_PWR_EN 6
#define GPIO_EXT_USWITCH_OFF 7

bool gpio_ext_setup(bool reset);
bool gpio_ext_enable(uint8_t bit);
bool gpio_ext_disable(uint8_t bit);
bool gpio_ext_uswitch_setup(bool reset);
bool gpio_ext_uswitch_enable(uint8_t bit);
bool gpio_ext_uswitch_disable(uint8_t bit);

#endif
