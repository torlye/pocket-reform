#ifndef _POCKET_UARTCOM_H
#define _POCKET_UARTCOM_H

#include <stdint.h>
#include "kvstore.h"

void uart_com_init();
void handle_uart_commands();

typedef struct uart_state_s {
  char remote_cmd;
  unsigned char cmd_state;
  unsigned int cmd_number;
  int echo;
  char kv_key[KV_KEY_STR_LEN];
  char kv_value[KV_VALUE_STR_LEN];
  int kv_key_len;
  int kv_value_len;
  int kv_count;
} uart_state_s;

#endif
