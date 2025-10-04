#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "fusb302b.h"
#include "pd.h"
#include "pd_com.h"
// FIXME: do not include the kitchen sink here
#include "sysctl.h"

static void print_src_fixed_pdo(int number, uint32_t pdo)
{
    unsigned int tmp;

    /* Voltage */
    unsigned int voltage = (pdo & PD_PDO_SRC_FIXED_VOLTAGE) >> PD_PDO_SRC_FIXED_VOLTAGE_SHIFT;

    /* Maximum Current */
    unsigned int i_a = (pdo & PD_PDO_SRC_FIXED_CURRENT) >> PD_PDO_SRC_FIXED_CURRENT_SHIFT;

    printf("pd_src_fixed_pdo#%d: V=%d.%02d Imax=%d.%02d",
        number, PD_PDV_V(voltage), PD_PDV_CV(voltage), PD_PDI_A(i_a), PD_PDI_CA(i_a));

    /* Peak Current */
    tmp = (pdo & PD_PDO_SRC_FIXED_PEAK_CURRENT) >> PD_PDO_SRC_FIXED_PEAK_CURRENT_SHIFT;
    if (tmp)
    {
        printf(" peak=%u", tmp);
    }

    /* Dual-Role Data */
    tmp = (pdo & PD_PDO_SRC_FIXED_DUAL_ROLE_DATA) >> PD_PDO_SRC_FIXED_DUAL_ROLE_DATA_SHIFT;
    if (tmp)
    {
        printf(" dual_role_data");
    }

    /* Dual-role power */
    tmp = (pdo & PD_PDO_SRC_FIXED_DUAL_ROLE_PWR) >> PD_PDO_SRC_FIXED_DUAL_ROLE_PWR_SHIFT;
    if (tmp)
    {
        printf(" dual_role_pwr");
    }

    /* USB Suspend Supported */
    tmp = (pdo & PD_PDO_SRC_FIXED_USB_SUSPEND) >> PD_PDO_SRC_FIXED_USB_SUSPEND_SHIFT;
    if (tmp)
    {
        printf(" usb_suspend");
    }

    /* USB Communications Capable */
    tmp = (pdo & PD_PDO_SRC_FIXED_USB_COMMS) >> PD_PDO_SRC_FIXED_USB_COMMS_SHIFT;
    if (tmp)
    {
        printf(" usb_comms");
    }

    /* Unchunked Extended Messages Supported */
    tmp = (pdo & PD_PDO_SRC_FIXED_UNCHUNKED_EXT_MSG) >> PD_PDO_SRC_FIXED_UNCHUNKED_EXT_MSG_SHIFT;
    if (tmp)
    {
        printf(" unchunked");
    }

    /* Unconstrained Power */
    tmp = (pdo & PD_PDO_SRC_FIXED_UNCONSTRAINED) >> PD_PDO_SRC_FIXED_UNCONSTRAINED_SHIFT;
    if (tmp)
    {
        printf(" unconstrained");
    }
    printf("\n");
}

unsigned int t = 0;

unsigned int pd_state;
bool pd_sent_soft_reset;
uint16_t pd_datarole = PD_DATAROLE_UFP;
uint16_t pd_powerrole = PD_POWERROLE_SINK;
static bool pd_datarole_changed = false;
static uint8_t pd_ccpin = 0;

int request_sent = 0;

union pd_msg tx;
int tx_id_count = 0;
union pd_msg rx_msg;
// in 10mA units
int requested_current = 0;

int factory_turn_on_once = 1;

void pd_init() {
  pd_state = PD_STATE_SETUP;
  pd_sent_soft_reset = 0;
}

unsigned int pd_get_state_for_debug() {
  return pd_state;
}

static void pd_set_fusb_switches() {
  uint8_t buf = 0 \
    | (pd_ccpin == 1 ? FUSB_SWITCHES0_MEAS_CC1 : 0) \
    | (pd_ccpin == 2 ? FUSB_SWITCHES0_MEAS_CC2 : 0) \
    | (pd_powerrole == PD_POWERROLE_SINK ? (FUSB_SWITCHES0_PDWN_2|FUSB_SWITCHES0_PDWN_1) : (FUSB_SWITCHES0_PU_EN1|FUSB_SWITCHES0_PU_EN2)) \
  ;
  fusb_write_byte(FUSB_SWITCHES0, buf);

  // Uses pd_ccpin as TXCC1/TXCC2.
  buf = 0 \
    | FUSB_SWITCHES1_AUTO_CRC
    | FUSB_SWITCHES1_SPECREV_REV2_0 \
    | ((pd_datarole == PD_DATAROLE_DFP) ? FUSB_SWITCHES1_DATAROLE_SRC_DFP : FUSB_SWITCHES1_DATAROLE_SNK_UFP) \
    | ((pd_powerrole == PD_POWERROLE_SOURCE) ? FUSB_SWITCHES1_POWERROLE : 0) \
    | pd_ccpin
  ;

  fusb_write_byte(FUSB_SWITCHES1, buf);
}

bool pd_tick(battery_info_s* battery_info) {
  if (pd_state == PD_STATE_SETUP) {
    // setup/timeout state
    if (battery_info->emergency_charge_necessary) {
      printf("# [pd] PD_STATE_SETUP - emergency_charge_necessary - not initializing PD\n");
      if (mps_reg_config.config0.chg_en != 1) {
        // 500mA, should be safe and get us to at least a minimal charge.
        charger_enable_charge(50);
      }
      return true;
    }
    charger_disable_charge();
    request_sent = 0;
    usb_host_5v_disable();

    printf("# [pd] PD_STATE_SETUP\n");
    // probe FUSB302BMPX
    uint8_t rxdata[2];
    if (i2c_read_timeout_us(i2c0, FUSB_ADDR, rxdata, 1, false, I2C_TIMEOUT)) {
      // SW_RES: Reset the FUSB302B including the I2C registers to their default values
      if (!fusb_write_byte(FUSB_RESET, FUSB_RESET_SW_RES))
        goto out;

      sleep_us(100);

      // enable toggle and DRP mode
      int mode;
      if (battery_info->som_is_powered) {
        mode = 1 << FUSB_CONTROL2_MODE_SHIFT;  // DRP
      } else {
        mode = 0b10 << FUSB_CONTROL2_MODE_SHIFT;  // SNK only
      }
      mps_reg_config.config0.susp_en = 1;
      mps_reg_config.config0.chg_en = 0;
      mps_write_byte(MPS_REG_CONFIG0, mps_reg_config.config0.reg_byte);

      if (!fusb_write_byte(FUSB_CONTROL2, FUSB_CONTROL2_TOGGLE | mode))
        goto out;

      // state is now:
      // Switches0 PDWN1=1 PDWN2=1
      // Switches1 SpecRev=Rev2.0
      // Measure MDAC=? 11_0001 =?
      // Slice SDAC_HYS=11 255mV
      // Control0 HOST_CUR=01 host pull up 80uA (Default) ; INT_MASK=1 All Interrupts masked
      // Control1 all zero
      // Control2 MODE=01 Enable DRP polling functionality if TOGGLE=1
      // Control3 N_RETRIES=11 (Three)   - AUTO_HARDRESET=0 AUTO_SOFTRESET=0
      // Mask all zero
      // Power PWR0=1 (Bandgap and wake circuit only)
      // Reset 0
      // OCPReg OCP_CUR=111 max range ; OCP_RANGE=1 OCP range between 100−800 mA
      // MaskA all zero
      // MaskB all zero
      // Control4 all zero
      // Status0a all zero
      // Status1a all zero
      // InterruptA
      // InterruptB
      // Status0 all zero
      // Status1 all zero
      // Interrupt
      // FIFOs

      // automatic retransmission + auto hard+soft reset
      if (!fusb_write_byte(FUSB_CONTROL3,
                           FUSB_CONTROL3_AUTO_HARDRESET |
                           FUSB_CONTROL3_AUTO_SOFTRESET |
                           (3<<FUSB_CONTROL3_N_RETRIES_SHIFT) |
                           FUSB_CONTROL3_AUTO_RETRY |
                           FUSB_CONTROL3_SEND_HARD_RESET
                           ))
        goto out;

      printf("# [pd] PD_STATE_SETUP done, going to PD_STATE_UNATTACHED\n");

      t = 0;
      pd_state = PD_STATE_UNATTACHED; // setup done

    } else {
      if (t > 1000) {
        printf("# [pd] PD_STATE_SETUP: fusb timeout.\n");
        t = 0;
      }
    }

  } else if (pd_state == PD_STATE_UNATTACHED) {
    // setup done, wait for toggle-done irq
    pd_sent_soft_reset = false;

    uint8_t i_irqa = 0;
    fusb_read_buf(FUSB_INTERRUPTA, 1, &i_irqa);

    if (i_irqa & FUSB_INTERRUPTA_I_TOGDONE) {
      uint8_t status1a = 0;
      fusb_read_buf(FUSB_STATUS1A, 1, &status1a);
      int togss = (status1a & FUSB_STATUS1A_TOGSS) >> FUSB_STATUS1A_TOGSS_SHIFT;
      if (togss == 5) {
        // SNK CC1
        printf("# [pd] PD_STATE_UNATTACHED -> SNK CC1, going to PD_STATE_UNATTACHED_SNK\n");
        pd_state = PD_STATE_UNATTACHED_SNK;
        pd_ccpin = 1;
      } else if (togss == 6) {
        // SNK CC2
        printf("# [pd] PD_STATE_UNATTACHED -> SNK CC2, going to PD_STATE_UNATTACHED_SNK\n");
        pd_state = PD_STATE_UNATTACHED_SNK;
        pd_ccpin = 2;
      } else if (togss == 1) {
        // SRC CC1
        printf("# [pd] PD_STATE_UNATTACHED -> SRC CC1, going to PD_STATE_UNATTACHED_SRC\n");
        pd_state = PD_STATE_UNATTACHED_SRC;
        pd_ccpin = 1;
      } else if (togss == 2) {
        // SRC CC2
        printf("# [pd] PD_STATE_UNATTACHED -> SRC CC2, going to PD_STATE_UNATTACHED_SRC\n");
        pd_state = PD_STATE_UNATTACHED_SRC;
        pd_ccpin = 2;
      } else {
        // Audio accessory or something else we do not understand. Reset.
        pd_state = PD_STATE_SETUP;
        t = 0;
        goto out;
      }

      // disable TOGGLE feature
      fusb_write_byte(FUSB_CONTROL2, 0);  // (1 << FUSB_CONTROL2_MODE_SHIFT));

      if (pd_state == PD_STATE_UNATTACHED_SNK) {
        pd_powerrole = PD_POWERROLE_SINK;
        pd_datarole = PD_DATAROLE_UFP;  // default for powerrole SINK
      } else {
        pd_powerrole = PD_POWERROLE_SOURCE;
        pd_datarole = PD_DATAROLE_DFP;  // default for powerrole SOURCE
        usb_host_5v_enable();
      }

      // Enable all FUSB blocks, including PD BMC and measure block.
      fusb_write_byte(FUSB_POWER, 0xF);

      pd_set_fusb_switches();
      pd_datarole_changed = false;

      if (pd_state == PD_STATE_UNATTACHED_SNK) {
        // enable VBUS measure for host/source attach detect.
        const int v = (3670 / 420) - 1;
        uint8_t measure = FUSB_MEASURE_MEAS_VBUS | v;
        fusb_write_byte(FUSB_MEASURE, measure);
        printf("[pd] PD_STATE_UNATTACHED_SNK measure=0x%02x\n", measure);
      }

      t = 0;
    }
  } else if (pd_state == PD_STATE_UNATTACHED_SNK) {
    // unattached.snk. Wait for VBUS to arrive.
    uint8_t status0;
    if (fusb_read_buf(FUSB_STATUS0, 1, &status0)) {
      if (status0 & FUSB_STATUS0_VBUSOK) {
        printf("# [pd] state PD_STATE_UNATTACHED_SNK VBUS is now OK\n");
        t = 0;
        pd_state = PD_STATE_ATTACHED_SNK;
      }
    }

    if (pd_state == PD_STATE_UNATTACHED_SNK && t > 10000) {
      // timeout
      // FIXME: timeout value?
      printf("# [pd] state PD_STATE_UNATTACHED_SNK - timeout waiting for VBUS\n");
      t = 0;
      pd_state = PD_STATE_SETUP; // FIXME: what state should we go to?
    }
  } else if (pd_state == PD_STATE_ATTACHED_SNK) {
    // attached.snk.
    // need to handshake charging capability and wait for ps_ok

    // detect detach by VBUS going away.
    uint8_t status0;
    if (fusb_read_buf(FUSB_STATUS0, 1, &status0) && (status0 & FUSB_STATUS0_VBUSOK) == 0) {
      printf("# [pd] state PD_STATE_ATTACHED_SNK VBUS went away\n");
      t = 0;
      pd_state = PD_STATE_SETUP;
      goto out;
    } else {
      if (pd_datarole_changed) {
        pd_set_fusb_switches();
        pd_datarole_changed = false;
      }
      // FIXME: this does not enforce the proper message order. maybe ok as is, maybe not.
      if (fusb_read_message(&rx_msg)) {
        uint8_t msgtype = PD_MSGTYPE_GET(&rx_msg);
        uint8_t numobj = PD_NUMOBJ_GET(&rx_msg);
        uint8_t msgrole = PD_POWERROLE_GET(&rx_msg);
        printf("# [pd] PD_STATE_ATTACHED_SNK: charger responds msg type: 0x%x msgrole: %d numobj: %d\n", msgtype, msgrole, numobj);
        if (msgrole == PD_POWERROLE_SOURCE && msgtype == PD_MSGTYPE_SOURCE_CAPABILITIES) {
          if (numobj == 0) {
            // FIXME: trigger a reset without sending a message first / or send reject?
            // TODO: figure out if this is actually caused by an overrun of the FUSB RX FIFO
            //usbpd_state = PD_STATE_SETUP;
          } else {
            int max_voltage = 0;
            int power_objects = 0;
            int pdo_current = 0;
            for (int i=0; i<numobj; i++) {
              uint32_t pdo = rx_msg.obj[i];

              if ((pdo & PD_PDO_TYPE) == PD_PDO_TYPE_FIXED) {
                print_src_fixed_pdo(i + 1, pdo);
                int voltage = PD_PDV_V(PD_PDO_SRC_FIXED_VOLTAGE_GET(pdo));
                // PD reports power in 10mA steps
                int current = PD_PDO_SRC_FIXED_CURRENT_GET(pdo);
                if (voltage > max_voltage && voltage <= 20 && current >= 10) {
                  power_objects = i+1;
                  max_voltage = voltage;
                  pdo_current = current;
                }
              } else {
                printf("# [pd] PD_STATE_ATTACHED_SNK not a fixed PDO: 0x%08lx\n", pdo);
              }
            }

            // FIXME: what about headroom for passing power to other USB devices?
            requested_current = pdo_current;
            if (requested_current > 300) {
              requested_current = 300;
            }

            printf("# [pd] requesting PO %d, %d V at %d mA\n", power_objects, max_voltage, requested_current * 10);
            tx.hdr = PD_MSGTYPE_REQUEST | PD_NUMOBJ(1) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_SPECREV_2_0;

            tx.hdr &= ~PD_HDR_MESSAGEID;
            tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;

            tx.obj[0] = PD_RDO_FV_MAX_CURRENT_SET(requested_current)
              | PD_RDO_FV_CURRENT_SET(requested_current)
              | PD_RDO_USB_COMMS
              | PD_RDO_NO_USB_SUSPEND
              | PD_RDO_OBJPOS_SET(power_objects);

            fusb_send_message(&tx);

            tx_id_count++;
          }
          t = 0;
        } else if (msgrole == PD_POWERROLE_SOURCE && msgtype == PD_MSGTYPE_ACCEPT) {
          printf("# [pd] charger accepted our requested PDO.\n");
          t = 0;
        } else if (msgrole == PD_POWERROLE_SOURCE && msgtype == PD_MSGTYPE_PS_RDY) {
          // power supply is ready
          printf("# [pd] power supply ready.\n");

          charger_enable_charge(requested_current);

          t = 0;
        } else if (msgrole == PD_POWERROLE_SOURCE && msgtype == PD_MSGTYPE_DR_SWAP) {
          // other side wants to swap data role.
          if (pd_datarole == PD_DATAROLE_DFP) {
            // we cannot switch away from DFP role. reject the message
            printf("# [pd] rejecting data-role swap\n");
            tx.hdr = PD_MSGTYPE_REJECT | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
            fusb_send_message(&tx);
          } else {
            // we started as UFP. Partner wants to become UFP.
            if (!battery_info->som_is_powered) {
              // SOM is not powered, so it will not act as a host. Tell partner to try later.
              printf("# [pd] replying with wait to data-role swap request\n");
              tx.hdr = PD_MSGTYPE_WAIT | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
              fusb_send_message(&tx);
            } else {
              // Accept. We become the DFP (host).
              printf("# [pd] accepting data-role swap\n");
              // TODO: switch pd_dr_role only after GOOD_CRC
              tx.hdr = PD_MSGTYPE_ACCEPT | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
              fusb_send_message(&tx);
              pd_datarole = PD_DATAROLE_DFP;
              pd_datarole_changed = true;
            }
          }
          t = 0;
        } else if (msgrole != PD_POWERROLE_SOURCE) {
          printf("# [pd] discarding non-source msg type: 0x%x numobj: %d\n", msgtype, numobj);
          t = 0;
        } else {
          printf("# [pd] msg type: 0x%x numobj: %d\n", msgtype, numobj);
          tx.hdr = PD_MSGTYPE_REJECT | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
          fusb_send_message(&tx);
        }
      } else if (t>10000 && !mps_reg_config.config0.chg_en) {
        // for some reason charging did not start.
        // TODO: send soft reset first.
        // TODO: fix timer.
        printf("# [pd] PD_STATE_ATTACHED_SNK timeout while handshaking, reset\n");
        t = 0;
        pd_state = PD_STATE_SETUP;
      } else if (t>8000 && !mps_reg_config.config0.chg_en && !pd_sent_soft_reset) {
        // Charging did not start.
        // This situation was observed with an Apple 30W charger, which apparently ignores a hard-reset
        // without a soft-reset and without an actual detach. Unclear why this happens.
        // Necessary to handle this so charging resumes after sysctl gets rebooted by a firmware upgrade.
        // TODO: the usbpd_sent_soft_reset stuff is not great.
        // TODO: fix timer.
        pd_sent_soft_reset = true;
        printf("# [pd] PD_STATE_ATTACHED_SNK timeout while handshaking, sending soft reset\n");
        tx.hdr = PD_MSGTYPE_SOFT_RESET | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
        fusb_send_message(&tx);
      }
    }

#ifdef FACTORY_MODE
    #pragma message "[mode] FACTORY MODE compiled in!"
    // in factory mode, turn on power immediately to be able to flash
    // the keyboard
    if (factory_turn_on_once) {
      factory_turn_on_once = 0;
      turn_som_power_on();
    }
#endif
  } else if (pd_state == PD_STATE_UNATTACHED_SRC) {
    // TODO: should do a lot of stuff, but for now keep USB2.0 devices happy
    if (t % 10000 == 0) {
      uint8_t status0;
      if (fusb_read_buf(FUSB_STATUS0, 1, &status0)) {
        printf("# [pd] state PD_STATE_UNATTACHED_SRC, status0 = %02x bc_lvl = %02x\n", status0, status0 & FUSB_STATUS0_BC_LVL);
        status0 &= FUSB_STATUS0_BC_LVL;
        if (status0 == 1) {
          // device is still connected, stay.
        } else {
          pd_state = PD_STATE_SETUP;
        }
        t = 0;
      }
    }

    if (t > 100000) {
      pd_state = PD_STATE_ATTACHED_SRC;
    }
  } else if (pd_state == PD_STATE_ATTACHED_SRC) {
    // TODO: everything
    // TODO: timeout

    pd_state = PD_STATE_SETUP;
  }


out:
  bool can_sleep = t > 0;
  t++;

  return can_sleep;
}
