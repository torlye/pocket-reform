#ifndef _POCKET_PD_COM_H
#define _POCKET_PD_COM_H

#include "sysctl.h"

typedef struct pd_state_s
{
  uint32_t ticks;
  uint8_t state;
  uint8_t next_state;

  bool request_sent;

  uint32_t tx_id_count;

  uint8_t power_object_index;
} pd_state_s;


// Enter: Host idle low power
#define PD_STATE_IDLE 0
#define PD_STATE_RESET_IDLE 1

#define PD_STATE_USB_DETECT 2
#define PD_STATE_USB_ATTACHED 3

#define PD_STATE_CHARGER_DETECT 4
#define PD_STATE_CHARGER_POWERED 7

void handle_pd_state(battery_info_s* battery_info, pd_state_s* pd_state);

#endif