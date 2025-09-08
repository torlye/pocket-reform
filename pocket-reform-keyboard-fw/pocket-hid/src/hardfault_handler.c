#include "pico/stdlib.h"
#include "hardware/watchdog.h"

__attribute__((naked)) void isr_hardfault(void) {
  watchdog_reboot(0, 0, 0);
}
