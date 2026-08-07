#ifndef HWAPI_POCKET
#define HWAPI_POCKET

#include "sysctl.h"

void hwapi_pocket_init(battery_info_s *binfo);
uint64_t hwapi_set_usb_mode(uint64_t port, uint64_t mode);

#endif
