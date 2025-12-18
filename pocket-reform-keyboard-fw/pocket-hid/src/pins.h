#ifndef _PINS_H
#define _PINS_H

/* FIXME build option */
#define VARIANT_RP2350A

#ifdef VARIANT_RP2350A

#define I2C_DEV i2c1

/* RP2350A Variant (Version 2) */

#define PIN_SDA 22
#define PIN_SCL 19

#define PIN_UART_TX 20
#define PIN_UART_RX 21

#define PIN_ROW1 12
#define PIN_ROW2 13
#define PIN_ROW3 14
#define PIN_ROW4 15
#define PIN_ROW5 16
#define PIN_ROW6 17

#define PIN_ROW_MASK ((1<<PIN_ROW1) | (1<<PIN_ROW2) | (1<<PIN_ROW3) \
                      | (1<<PIN_ROW4) | (1<<PIN_ROW5) | (1<<PIN_ROW6))

#define PIN_COL1 0
#define PIN_COL2 1
#define PIN_COL3 2
#define PIN_COL4 3
#define PIN_COL5 4
#define PIN_COL6 5
#define PIN_COL7 6
#define PIN_COL8 7
#define PIN_COL9 8
#define PIN_COL10 9
#define PIN_COL11 10
#define PIN_COL12 11

#define PIN_COL_MASK ((1<<PIN_COL1) | (1<<PIN_COL2) | (1<<PIN_COL3) \
                      | (1<<PIN_COL4) | (1<<PIN_COL5) | (1<<PIN_COL6) \
                      | (1<<PIN_COL7) | (1<<PIN_COL8) | (1<<PIN_COL9) \
                      | (1<<PIN_COL10) | (1<<PIN_COL11) | (1<<PIN_COL12))

#define PIN_LEDS 18
#define PIN_LED_EN 28

/* charger for wireless operation */

#define PIN_CHG_STAT1 26
#define PIN_CHG_STAT2 27

/* wireless module */

#define PIN_WL_ON  23
#define PIN_WL_D   24
#define PIN_WL_CS  25
#define PIN_WL_CLK 29

#else

/* RP2040 Variant (Version 1) */

#define I2C_DEV i2c0

#define PIN_SDA 0
#define PIN_SCL 1

#define PIN_UART_TX 4
#define PIN_UART_RX 5

#define PIN_ROW1 19
#define PIN_ROW2 20
#define PIN_ROW3 23
#define PIN_ROW4 22
#define PIN_ROW5 21
#define PIN_ROW6 18

#define PIN_ROW_MASK ((1<<PIN_ROW1) | (1<<PIN_ROW2) | (1<<PIN_ROW3) \
                      | (1<<PIN_ROW4) | (1<<PIN_ROW5) | (1<<PIN_ROW6))

#define PIN_COL1 6
#define PIN_COL2 7
#define PIN_COL3 8
#define PIN_COL4 9
#define PIN_COL5 10
#define PIN_COL6 11
#define PIN_COL7 12
#define PIN_COL8 13
#define PIN_COL9 14
#define PIN_COL10 15
#define PIN_COL11 16
#define PIN_COL12 17

#define PIN_COL_MASK ((1<<PIN_COL1) | (1<<PIN_COL2) | (1<<PIN_COL3) \
                      | (1<<PIN_COL4) | (1<<PIN_COL5) | (1<<PIN_COL6) \
                      | (1<<PIN_COL7) | (1<<PIN_COL8) | (1<<PIN_COL9) \
                      | (1<<PIN_COL10) | (1<<PIN_COL11) | (1<<PIN_COL12))

#define PIN_LEDS 24

#endif
#endif
