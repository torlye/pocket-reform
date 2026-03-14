#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "fusb302b.h"
#include "pd.h"
#include "pd_com.h"
// FIXME: do not include the kitchen sink here
#include "sysctl.h"
#include "mntre_usbids.h"

static void print_src_fixed_pdo(int number, uint32_t pdo)
{
    unsigned int tmp;

    /* Voltage */
    unsigned int voltage = PD_PDO_SRC_FIXED_VOLTAGE_GET(pdo);

    /* Maximum Current */
    unsigned int current = PD_PDO_SRC_FIXED_CURRENT_GET(pdo);

    printf("pd_src_fixed_pdo#%d: V=%d.%02d Imax=%d.%02d",
        number, PD_PDV_V(voltage), PD_PDV_CV(voltage), PD_PDI_A(current), PD_PDI_CA(current));

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
unsigned int requested_current = 0;

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

struct picked_pdo {
  unsigned int pdo_num;             // PDO number suitable for PD message. 0 = invalid.
  unsigned int voltage;             // V
  unsigned int max_current;         // 10mA
  unsigned int max_power;           // 10mW
};

static struct picked_pdo pick_pdo(union pd_msg *msg) {
  unsigned int numobj = PD_NUMOBJ_GET(msg);
  struct picked_pdo picked = {0};

  for (unsigned int i = 0; i < numobj; i++) {
    uint32_t pdo = msg->obj[i];

    if ((pdo & PD_PDO_TYPE) == PD_PDO_TYPE_FIXED) {
      print_src_fixed_pdo(i + 1, pdo);

      unsigned int voltage = PD_PDV_V(PD_PDO_SRC_FIXED_VOLTAGE_GET(pdo));
      if (voltage > 20) {
        // our charger IC is limited to 20V input.
        continue;
      }

      // PD reports power in 10mA steps
      unsigned int max_current = PD_PDO_SRC_FIXED_CURRENT_GET(pdo);
      if (max_current < 10) {
        // less than 100mA (@20V = 2W) is not good enough for charging or running, do not bother.
        continue;
      }

      unsigned int max_power = voltage * max_current;

      // PDO selection logic:
      // 1) try to pick a PDO with >= 9V. Below that, charging is slow,
      //    especially on machines with the diode on the input path, forcing the MP2650 into the
      //    slow path.
      // 2) try to pick the highest available power at the highest voltage.
      //    give some leeway for slightly smaller power at the higher voltage. this can be
      //    necessary with some chargers, f.e. loaded Apple 35W 2-port charger can report
      //    slightly higher power at 9V than at 20V. but then we still want 20V.
      if (voltage > picked.voltage
          && (
             picked.voltage < 9
          || picked.max_power < 10
          || max_power >= (picked.max_power - 10)
        )
      ) {
        picked.pdo_num = i + 1;
        picked.voltage = voltage;
        picked.max_current = max_current;
        picked.max_power = max_power;
      }
    } else {
      printf("# [pd] not a fixed PDO: 0x%08lx\n", pdo);
    }
  }

  return picked;
}

static void pd_send_not_supported() {
  printf("# [pd] tx PD_MSGTYPE_C_NOT_SUPPORTED\n");
  tx.hdr = PD_MSGTYPE_C_NOT_SUPPORTED | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
  fusb_send_message(&tx);
}

// Returns if state was "changed" in some form and we expect to maybe be called again.
static bool pd_comm_pd(battery_info_s* battery_info) {
  if (pd_datarole_changed) {
    pd_set_fusb_switches();
    pd_datarole_changed = false;
  }
  // FIXME: this does not enforce the proper message order. maybe ok as is, maybe not.
  if (!fusb_read_message(&rx_msg)) {
    return false;
  }

  unsigned int msgtype = PD_MSGTYPE_GET(&rx_msg);
  unsigned int msgrole = PD_POWERROLE_GET(&rx_msg);
  unsigned int numobj = PD_NUMOBJ_GET(&rx_msg);

  if (msgrole == PD_POWERROLE_SOURCE) {

    if (rx_msg.hdr & PD_HDR_EXT) {
      // extended messages.
      pd_send_not_supported();
      return false;
    }

    if (numobj == 0) {
      // control messages
      switch (msgtype) {
      case PD_MSGTYPE_C_GOODCRC:
        // TODO: we should care about these in some situations.
        return false;

      case PD_MSGTYPE_C_ACCEPT:
        printf("# [pd] charger accepted our requested PDO.\n");
        return true;

      case PD_MSGTYPE_C_PS_RDY:
        // power supply is ready
        printf("# [pd] power supply ready.\n");
        charger_enable_charge(requested_current);
        return true;

      case PD_MSGTYPE_C_DR_SWAP:
        // other side wants to swap data role.
        if (pd_datarole == PD_DATAROLE_DFP) {
          // we cannot switch away from DFP role. reject the message
          printf("# [pd] rejecting data-role swap\n");
          tx.hdr = PD_MSGTYPE_C_REJECT | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
          fusb_send_message(&tx);
        } else {
          // we started as UFP. Partner wants to become UFP.
          if (!battery_info->som_is_powered) {
            // SOM is not powered, so it will not act as a host. Tell partner to try later.
            printf("# [pd] replying with wait to data-role swap request\n");
            tx.hdr = PD_MSGTYPE_C_WAIT | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
            fusb_send_message(&tx);
          } else {
            // Accept. We become the DFP (host).
            printf("# [pd] accepting data-role swap\n");
            // TODO: switch pd_dr_role only after GOOD_CRC
            tx.hdr = PD_MSGTYPE_C_ACCEPT | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
            fusb_send_message(&tx);
            pd_datarole = PD_DATAROLE_DFP;
            pd_datarole_changed = true;
          }
        }
        return true;

      case PD_MSGTYPE_C_PR_SWAP:
        // power role swap. not implemented.
        printf("# [pd] rejecting power-role swap\n");
        tx.hdr = PD_MSGTYPE_C_REJECT | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
        fusb_send_message(&tx);
        return false;

      case PD_MSGTYPE_C_VCONN_SWAP:
      default:
        pd_send_not_supported();
        return false;
      }

    } else if (msgtype != PD_MSGTYPE_D_VENDOR_DEFINED) {
      // "standard" data messages
      // data messages
      switch (msgtype) {
      case PD_MSGTYPE_D_SOURCE_CAPABILITIES:
      {
        struct picked_pdo picked = pick_pdo(&rx_msg);
        // FIXME: what about headroom for passing power to other USB devices?
        requested_current = picked.max_current;
        if (requested_current > 300) {
          requested_current = 300;
        }

        printf("# [pd] requesting PDO %u, %u V (max %u mA) at %u mA\n", picked.pdo_num, picked.voltage, picked.max_current * 10, requested_current * 10);

        tx.hdr = PD_MSGTYPE_D_REQUEST | PD_NUMOBJ(1) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_SPECREV_2_0;

        tx.hdr &= ~PD_HDR_MESSAGEID;
        tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;

        tx.obj[0] = PD_RDO_FV_MAX_CURRENT_SET(requested_current)
          | PD_RDO_FV_CURRENT_SET(requested_current)
          | PD_RDO_USB_COMMS
          | PD_RDO_NO_USB_SUSPEND
          | PD_RDO_OBJPOS_SET(picked.pdo_num);

        fusb_send_message(&tx);

        tx_id_count++;

        return true;
      }

      case PD_MSGTYPE_D_REQUEST:  // only from sinks
      default:
        pd_send_not_supported();
        return false;
      }
    } else {
      // "Vendor"-specific data message
      uint32_t vdm_header = rx_msg.obj[0];
      uint16_t vsid = (vdm_header >> PD_VSID__SHIFT) & 0xFFFF;
      printf("# [pd] vdm_header = 0x%lx vsid: %x\n", vdm_header, vsid);

      if ((vdm_header & PD_VDM_HEADER_STRUCTURED) == 0) {
        // Unstructured message, which we cannot parse.
        pd_send_not_supported();
        return false;
      }
      if ((vdm_header & 0x00E0) != 0) {
        // non-REQ message, which we cannot parse. ignore it.
        return false;
      }

      switch (vsid) {
      case PD_VSID_USBPD:  // USB-PD Standard "Vendor" ID
      {
        switch (vdm_header & 0x1F) {
        case PD_VDM_USBPD_DISCOVER_IDENTITY:
          printf("# [pd] replying to Discover Identity\n");
          tx.hdr = PD_MSGTYPE_D_VENDOR_DEFINED | PD_NUMOBJ(4) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_SPECREV_2_0;
          tx.hdr &= ~PD_HDR_MESSAGEID;
          tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;

          // VDM Header
          tx.obj[0] = (PD_VSID_USBPD << PD_VSID__SHIFT) | PD_VDM_HEADER_STRUCTURED | PD_VDM_HEADER_TYPE_ACK | PD_VDM_USBPD_DISCOVER_IDENTITY;
          // ID VDO: USB Host capable; Product Type (DFP) = 010; Connector Type = 10; plus VID
          tx.obj[1] = 0x81400000 | USB_VID_PIDCODES;
          // Certification stat VDO
          tx.obj[2] = 0;
          // Product VDO: v1.0; plus PID
          tx.obj[3] = 0x01000000 | USB_PID_MNT_POCKET_REFORM_SYSCTL_10;

          fusb_send_message(&tx);
          tx_id_count++;

          return true;
        default:
          pd_send_not_supported();
          return false;
        }
      }
      default:
        pd_send_not_supported();
        return false;
      }
    }

  } else {
    // msgrole == PD_POWERROLE_SINK
    pd_send_not_supported();
    return false;
  }
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

      busy_wait_us(100);

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

      // unmask all interrupts to be able to wake from
      // dormant mode via USB-C events
      fusb_write_byte(FUSB_CONTROL0, 0b10 << FUSB_CONTROL0_HOST_CUR_SHIFT);

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
      if (pd_comm_pd(battery_info)) {
        t = 0;
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
        tx.hdr = PD_MSGTYPE_C_SOFT_RESET | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
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

#if 0
    if (pd_comm_pd(battery_info)) {
      t = 0;
    }
#endif

    if (t == 1 || t % 10000 == 0) {
#if 0
      printf("# [pd] state PD_STATE_UNATTACHED_SRC - sending PD_MSGTYPE_D_SOURCE_CAPABILITIES\n");
      tx.hdr = PD_MSGTYPE_D_SOURCE_CAPABILITIES | PD_NUMOBJ(1) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT);
      tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
      tx.obj[0] = 0x26019064;  // 5V/1A with DRP and DRD
      fusb_send_message(&tx);
#endif

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
