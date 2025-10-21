#ifndef _PINS_H
#define _PINS_H

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
