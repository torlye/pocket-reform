#include <stdbool.h>
#include <stdint.h>

#ifndef POCKET_PD_COM_H
#define POCKET_PD_COM_H

#define PD_STATE_SETUP 0
#define PD_STATE_UNATTACHED 1
#define PD_STATE_UNATTACHED_SNK 2
#define PD_STATE_ATTACHED_SNK 3
#define PD_STATE_UNATTACHED_SRC 4
#define PD_STATE_ATTACHED_SRC 5

struct battery_info_s;

void pd_init();
bool pd_tick(struct battery_info_s* battery_info);
unsigned int pd_get_state_for_debug();
void send_vdm(uint32_t message_type, uint8_t prime);
void send_vdm2(uint32_t obj0, uint32_t obj1);
void send_source_cap(uint8_t prime);
void pd_send_reset();
void pd_set_max_voltage_req(uint64_t v);
void pd_set_force_sink(bool force);

#endif
