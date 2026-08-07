/**
 * Simple key-value store for stringly settings
 */

#include <string.h>
#include <stdint.h>
#include "kvstore.h"

// NOTE: so far just a in-memory sketch. major TODO: persist at the end of flash memory
// TODO optimize by splitting into number+string tables
static struct kv_entry kv_mem[KV_MAX_ENTRIES];

// returns -1 if key could not be found
int kv_lookup_str(uint8_t* key, uint8_t key_len) {
	if (key_len > KV_MAX_NAME_LEN || !key) return -1;

	for (int i = 0; i < KV_MAX_ENTRIES; i++) {
		if (kv_mem[i].key_len == key_len && !memcmp(key, &kv_mem[i].key, key_len)) {
			// found
			return i;
		}
	}
	return -1;
}

struct kv_entry* kv_get(uint8_t slot) {
	if (slot >= KV_MAX_ENTRIES) return NULL;
	return &kv_mem[slot];
}

// buf must be at least KV_MAX_NAME_LEN+1 in size!
int kv_get_key_str(uint8_t slot, char* buf) {
	if (slot >= KV_MAX_ENTRIES) return 0;
	int len = kv_mem[slot].key_len;
	// TODO: error handling for corrupted entry
	if (len >= KV_MAX_NAME_LEN) return 0;
	memcpy(buf, kv_mem[slot].key, len);
	buf[len] = 0;
	return len;
}

int kv_get_value_str(uint8_t slot, char* buf) {
	if (slot >= KV_MAX_ENTRIES) return 0;
	int len = kv_mem[slot].value_len;
	// TODO: error handling for corrupted entry
	if (len >= KV_MAX_VALUE_LEN) return 0;
	memcpy(buf, kv_mem[slot].value, len);
	buf[len] = 0;
	return len;
}

// returns -1 if value could not be stored (out of space)
// returns key index if value could be stored
int kv_update_str(uint8_t* key, uint8_t key_len, uint8_t* value, uint8_t value_len) {
	if (key_len > KV_MAX_NAME_LEN || !key) return -1;
	if (value_len > KV_MAX_VALUE_LEN || !value) return -1;

	int target_slot = -1;
	for (int i = 0; i < KV_MAX_ENTRIES; i++) {
		if (kv_mem[i].key_len == key_len && !memcmp(key, &kv_mem[i], key_len)) {
			// found, overwrite
			target_slot = i;
			break;
		}
	}
	if (target_slot == -1) {
		// not found, allocate new slot
		for (int i = 0; i < KV_MAX_ENTRIES; i++) {
			if (kv_mem[i].key_len == 0) {
				target_slot = i;
				break;
			}
		}
	}
	if (target_slot == -1) {
		// no more free slots, error
		return -1;
	}
	// update key and value
	memcpy(&kv_mem[target_slot].value, value, value_len);
	kv_mem[target_slot].value_len = value_len;
	memcpy(&kv_mem[target_slot].key, key, key_len);
	kv_mem[target_slot].key_len = key_len;
	return target_slot;
}
