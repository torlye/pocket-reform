#ifndef _POCKET_KVSTORE_H
#define _POCKET_KVSTORE_H

#include <stdint.h>

#define KV_MAX_NAME_LEN 16
// including 1 byte for pascal-style string length!
#define KV_MAX_VALUE_LEN 32
#define KV_MAX_ENTRIES 128
#define KV_SLOT_SZ (KV_MAX_NAME_LEN+KV_MAX_VALUE_LEN)
#define KV_KEY_STR_LEN (KV_MAX_NAME_LEN+1)
#define KV_VALUE_STR_LEN (KV_MAX_VALUE_LEN+1)

struct kv_entry {
  uint8_t key_len;
  uint8_t key[KV_MAX_NAME_LEN];
  uint8_t value_len;
  uint8_t value[KV_MAX_VALUE_LEN];
};

int kv_lookup_str(uint8_t* key, uint8_t key_len);
int kv_update_str(uint8_t* key, uint8_t key_len, uint8_t* value, uint8_t value_len);
int kv_get_key_str(uint8_t slot, char* buf);
int kv_get_value_str(uint8_t slot, char* buf);
struct kv_entry* kv_get(uint8_t slot);

#endif
