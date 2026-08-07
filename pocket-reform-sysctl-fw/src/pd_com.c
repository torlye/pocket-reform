#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "fusb302b.h"
#include "hardware/timer.h"
#include "pd.h"
#include "pd_com.h"
// FIXME: do not include the kitchen sink here
#include "sysctl.h"
#include "mntre_usbids.h"
#include "altmode.h"

static void print_src_fixed_pdo(int number, uint32_t pdo)
{
  unsigned int tmp;

  /* Voltage */
  unsigned int voltage = PD_PDO_SRC_FIXED_VOLTAGE_GET(pdo);

  /* Maximum Current */
  unsigned int current = PD_PDO_SRC_FIXED_CURRENT_GET(pdo);

  printf("# [pd]   pd_src_fixed_pdo#%d: V=%d.%02d Imax=%d.%02d",
         number, PD_PDV_V(voltage), PD_PDV_CV(voltage), PD_PDI_A(current), PD_PDI_CA(current));

  /* Peak Current */
  tmp = (pdo & PD_PDO_SRC_FIXED_PEAK_CURRENT) >> PD_PDO_SRC_FIXED_PEAK_CURRENT_SHIFT;
  if (tmp) printf(" peak=%u", tmp);

  /* Dual-Role Data */
  tmp = (pdo & PD_PDO_SRC_FIXED_DUAL_ROLE_DATA) >> PD_PDO_SRC_FIXED_DUAL_ROLE_DATA_SHIFT;
  if (tmp) printf(" dual_role_data");

  /* Dual-role power */
  tmp = (pdo & PD_PDO_SRC_FIXED_DUAL_ROLE_PWR) >> PD_PDO_SRC_FIXED_DUAL_ROLE_PWR_SHIFT;
  if (tmp) printf(" dual_role_pwr");

  /* USB Suspend Supported */
  tmp = (pdo & PD_PDO_SRC_FIXED_USB_SUSPEND) >> PD_PDO_SRC_FIXED_USB_SUSPEND_SHIFT;
  if (tmp) printf(" usb_suspend");

  /* USB Communications Capable */
  tmp = (pdo & PD_PDO_SRC_FIXED_USB_COMMS) >> PD_PDO_SRC_FIXED_USB_COMMS_SHIFT;
  if (tmp) printf(" usb_comms");

  /* Unchunked Extended Messages Supported */
  tmp = (pdo & PD_PDO_SRC_FIXED_UNCHUNKED_EXT_MSG) >> PD_PDO_SRC_FIXED_UNCHUNKED_EXT_MSG_SHIFT;
  if (tmp) printf(" unchunked");

  /* Unconstrained Power */
  tmp = (pdo & PD_PDO_SRC_FIXED_UNCONSTRAINED) >> PD_PDO_SRC_FIXED_UNCONSTRAINED_SHIFT;
  if (tmp) printf(" unconstrained");
  printf("\n");
}

unsigned int t = 0;

unsigned int pd_state;
bool pd_sent_soft_reset;
uint16_t pd_datarole = PD_DATAROLE_UFP;
uint16_t pd_powerrole = PD_POWERROLE_SINK;
static uint8_t pd_ccpin = 0;
static uint8_t pd_host_current = 0b10; // default host (source) pullup current

int request_sent = 0;

union pd_msg tx;
static int tx_id_count = 0;
union pd_msg rx_msg;
// in 10mA units
unsigned int requested_current = 0;
static unsigned int max_voltage_requested = 20;
static int source_pdo_accept_sent = 0;
static int source_pdo_ready_sent = 0;
static int source_pdo_ready_acked = 0;
static bool alt_mode_requested = false;
static bool pd_force_sink = false;
static bool pd_source_cap_acked = false;

int factory_turn_on_once = 1;

void pd_set_max_voltage_req(uint64_t v) {
  if (v > 20 || v < 5) return;
  max_voltage_requested = v;
}

void pd_init() {
  pd_state = PD_STATE_SETUP;
  pd_sent_soft_reset = 0;
}

unsigned int pd_get_state_for_debug() {
  return pd_state;
}

void pd_set_force_sink(bool force) {
  pd_force_sink = force;
}

#define PD_VERSION PD_SPECREV_2_0

void send_source_cap(uint8_t prime) {
  tx_id_count = 0;
  printf("# [pd] send_source_cap\n");
  tx.hdr = PD_MSGTYPE_D_SOURCE_CAPABILITIES | PD_NUMOBJ(1) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
  tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
  tx.obj[0] = 0x26019064;  // 5V/1A with DRP and DRD
  //tx.obj[0] = 0x36019096;

  int res = -1;
  if (prime == 2) {
    res = fusb_send_message_prime_prime(&tx);
  }
  else if (prime == 1) {
    res = fusb_send_message_prime(&tx);
  }
  else {
    res = fusb_send_message(&tx);
  }
  printf("# [pd]   result: %d\n", res);
  tx_id_count++;
}

void send_sink_cap(uint8_t prime) {
  tx_id_count = 0;
  printf("# [pd] send_sink_cap\n");
  tx.hdr = PD_MSGTYPE_D_SINK_CAPABILITIES | PD_NUMOBJ(2) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
  tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
  tx.obj[0] = 0x26019064;  // 5V/1A with DRP and DRD
  tx.obj[1] = PD_PDO_TYPE_FIXED | (20 << PD_PDO_SNK_FIXED_VOLTAGE_SHIFT) | (2 << PD_PDO_SNK_FIXED_CURRENT_SHIFT) | PD_PDO_SNK_FIXED_DUAL_ROLE_DATA | PD_PDO_SNK_FIXED_USB_COMMS | PD_PDO_SNK_FIXED_DUAL_ROLE_PWR;

  int res = -1;
  if (prime == 2) {
    res = fusb_send_message_prime_prime(&tx);
  }
  else if (prime == 1) {
    res = fusb_send_message_prime(&tx);
  }
  else {
    res = fusb_send_message(&tx);
  }
  printf("# [pd]   result: %d\n", res);
  tx_id_count++;
}

void send_source_accept() {
  printf("# [pd]   send_source_accept\n");
  tx.hdr = PD_MSGTYPE_C_ACCEPT | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
  tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
  fusb_send_message(&tx);
  tx_id_count++;
}

void send_ps_ready() {
  printf("# [pd]   send_ps_ready\n");
  tx.hdr = PD_MSGTYPE_C_PS_RDY | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
  tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
  fusb_send_message(&tx);
  tx_id_count++;
}

void send_vdm([[maybe_unused]] uint32_t message_type, uint8_t prime) {
  tx.hdr = PD_MSGTYPE_D_VENDOR_DEFINED | PD_NUMOBJ(1) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
  tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
  tx.obj[0] = message_type;

  printf("# [pd]   send VDM: hdr: 0x%08x obj0: 0x%08lx prime: %d datarole: %s powerrole: %s\n", tx.hdr, tx.obj[0], prime,
         pd_datarole == PD_DATAROLE_DFP?"DFP":"UFP", pd_powerrole == PD_POWERROLE_SINK?"SINK":"SRC");

  if (prime == 2) {
    fusb_send_message_prime_prime(&tx);
  }
  else if (prime == 1) {
    fusb_send_message_prime(&tx);
  }
  else {
    fusb_send_message(&tx);
  }

  tx_id_count++;
}

void send_vdm2([[maybe_unused]] uint32_t message_type, uint32_t obj1) {
  tx.hdr = PD_MSGTYPE_D_VENDOR_DEFINED | PD_NUMOBJ(2) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
  tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
  tx.obj[0] = message_type;
  tx.obj[1] = obj1;

  printf("# [pd]   send VDM2: hdr: 0x%08x obj0: 0x%08lx obj1: 0x%08lx datarole: %s powerrole: %s\n", tx.hdr, tx.obj[0], tx.obj[1],
         pd_datarole == PD_DATAROLE_DFP?"DFP":"UFP", pd_powerrole == PD_POWERROLE_SINK?"SINK":"SRC");

  fusb_send_message(&tx);
  tx_id_count++;
}

void pd_send_reset() {
  printf("# [pd]   send reset\n");
  tx.hdr = PD_MSGTYPE_C_SOFT_RESET | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
  tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
  bool res = fusb_send_message(&tx);
  tx_id_count++;
  printf("# [pd]   sent reset, result: %d\n", res);

  pd_state = PD_STATE_SETUP;
}

static bool enable_vconn = false;
static bool pd_apply_datarole() {
  if (pd_datarole == PD_DATAROLE_DFP) {
    // we're USB host, turn off USB UART
    //printf("# [pd] pd_apply_datarole: DFP\n");
    return gpio_ext_uswitch_disable(3);
  } else {
    // we're USB device, turn on USB UART
    //printf("# [pd] pd_apply_datarole: UFP\n");
    return gpio_ext_uswitch_enable(3);
  }
  return true;
}

static void pd_set_fusb_switches() {
  uint8_t buf = 0;

  /*
    - A downstream facing port (DFP), such as a host computer, exposes pull-up terminations, Rp, on its CC pins (CC1 and CC2)
    - An upstream facing port (UFP), such as a peripheral, exposes pull-down terminations, Rd, on its CC pins.
    - The purpose of Rp and Rd terminations on CC pins is to identify the DFP to UFP connection and the CC pin that will be used for communication. To do this, the DFP monitors both CC pins for a voltage lower than its unterminated voltage.

    - UFP’s Rd value is fixed at 5.1 kΩ.
    - The Type-C cable needs to expose a pull-down termination, Ra, on its VCONN pin to signal to the DFP that it needs power [800 - 1.2k ohms]
    - The DFP must be able to differentiate between the presence of Rd and Ra to know whether there is a UFP attached and where to apply VCONN. The DFP is not required to source VCONN unless Ra is detected.

    source: https://community.infineon.com/t5/Knowledge-Base-Articles/USB-Type-C-connector-Rp-Rd-and-Ra-termination-resistors/ta-p/253544

    - a Source is a USB Power Delivery Port supplying power; on an attach event, it assumes the DFP and VCONN Source roles
    - a Sink is a USB Power Delivery Port consuming power; on an attach event, it assumes the UFP role
    - a Dual-Role Power Port (DRP) supports both Source and Sink roles
    - The Source and Sink roles, DFP and UFP roles, and the VCONN Source role can all be subsequently swapped

    source: file:///home/minute/Downloads/ta0357-overview-of-usb-typec-and-power-delivery-technologies-stmicroelectronics.pdf
   */

  // Configure SWITCHES0
  // TODO: to detect Ra (e-marker cable), we have to have at least host_cur b10.
  // see fusb302 datasheet p. 6 and 7
  // MDAC setting is referred to as "COMP Setting" there
  // comp == 0 -> attach
  // comp == 1 -> detach
  uint8_t mdac_setting = 0b110100;
  uint8_t meas_vbus = 1;
  if (pd_powerrole == PD_POWERROLE_SOURCE) {
    meas_vbus = 0;
    // TODO Ra termination
    //if (pd_host_current == 0b10) {
    //  // 0.42V
    //  mdac_setting = 0b001001;
    //} else if (pd_host_current == 0b11) {
    //  // 0.8V
    //  mdac_setting = 0b010010;
    //}
    // Rd termination
    if (pd_host_current <= 0b10) {
      // 1.6V
      mdac_setting = 0b100101;
    } else {
      // 2.6V
      mdac_setting = 0b111101;
    }
    usb_host_5v_set(0,1);
  } else {
    meas_vbus = 1;
    mdac_setting = 0b110100;
    usb_host_5v_set(0,0);
  }

  // 20260716: active cables don't work if MEAS is enabled on the other CC pin!
  if (pd_ccpin == 1) {
    buf |= FUSB_SWITCHES0_MEAS_CC1;
  } else if (pd_ccpin == 2) {
    buf |= FUSB_SWITCHES0_MEAS_CC2;
  }
  if (pd_powerrole == PD_POWERROLE_SINK) {
    buf |= FUSB_SWITCHES0_PDWN_1 | FUSB_SWITCHES0_PDWN_2;
  }
  if (pd_powerrole == PD_POWERROLE_SOURCE) {
    buf |= FUSB_SWITCHES0_PU_EN1 | FUSB_SWITCHES0_PU_EN2;
  }

  if (enable_vconn) {
    //printf("# [pd switches] enabling vconn\n");
    if (pd_ccpin == 1) {
      // CC1 used for comms, so CC2 used for VCONN
      buf |= FUSB_SWITCHES0_VCONN_CC2;
      // disable pullup on CC2 TODO: not sure if necessary
      buf = buf & ~FUSB_SWITCHES0_PU_EN2;
    } else if (pd_ccpin == 2) {
      // CC2 used for comms, so CC1 used for VCONN
      buf |= FUSB_SWITCHES0_VCONN_CC1;
      // disable pullup on CC1 TODO: not sure if necessary
      buf = buf & ~FUSB_SWITCHES0_PU_EN1;
    }
  }

  fusb_write_byte(FUSB_SWITCHES0, buf);

  // Configure SWITCHES1
  // Uses pd_ccpin as TXCC1/TXCC2.
  buf = 0
        | FUSB_SWITCHES1_AUTO_CRC
        | FUSB_SWITCHES1_SPECREV_REV2_0
        | ((pd_datarole == PD_DATAROLE_DFP) ?
           FUSB_SWITCHES1_DATAROLE_SRC_DFP : FUSB_SWITCHES1_DATAROLE_SNK_UFP)
        | ((pd_powerrole == PD_POWERROLE_SOURCE) ?
           FUSB_SWITCHES1_POWERROLE : 0)
        | pd_ccpin
  ;

  fusb_write_byte(FUSB_SWITCHES1, buf);

  // Configure MDAC
  fusb_write_byte(FUSB_MEASURE, mdac_setting|(meas_vbus<<6));

  pd_apply_datarole();
  if (mb_version() >= 2) {
    // TODO: too early?
    if (pd_ccpin == 1) {
      altmode_enable_dp(0);
    } else {
      altmode_enable_dp(1);
    }
  }
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
      // TODO: temp hack for vbus1=vbus2 bodge on mb2 proto
      if (voltage > max_voltage_requested) {
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
          && voltage <= max_voltage_requested
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
      printf("# [pd]   not a fixed PDO: 0x%08lx\n", pdo);
    }
  }

  return picked;
}

static void pd_send_not_supported() {
  printf("# [pd] tx PD_MSGTYPE_C_NOT_SUPPORTED\n");
  tx.hdr = PD_MSGTYPE_C_NOT_SUPPORTED | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
  tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
  fusb_send_message(&tx);
  tx_id_count++;
}

#define PD_ALT_MODE_PINS_E 0b00000100
#define PD_ALT_MODE_PINS_D 0b00001000

static int alt_mode_pin_assignment = 0;

static bool pd_handle_vdm_response() {
  int numobj = PD_NUMOBJ_GET(&rx_msg);
  uint32_t vdm_header = rx_msg.obj[0];
  uint32_t obj1 = rx_msg.obj[1];
  uint16_t vsid = (vdm_header >> PD_VSID__SHIFT) & 0xFFFF;
  printf("# [pd] pd_handle_vdm_response vdm_header: 0x%lx vsid: %x numobj: %d\n", vdm_header, vsid, numobj);

  for (int i=0; i<numobj; i++) {
    printf("# [pd]   vdm: obj %d: 0x%lx\n", i, rx_msg.obj[i]);
  }

  uint8_t vdm_type = vdm_header & 0x1f;

  if (vdm_header & PD_VDM_HEADER_TYPE_ACK) {
    if (vsid == PD_VSID_USBPD) {
      if (vdm_type == PD_VDM_USBPD_DISCOVER_IDENTITY) {
        printf("# [pd]   vdm: ACK discover_identity\n");
      }
      if (vdm_type == PD_VDM_USBPD_DISCOVER_SVIDS) {
        printf("# [pd]   vdm: ACK discover_svids\n");

        // send discover modes
        send_vdm(0xff018003, 0);
        // TODO state machine
        alt_mode_requested = true;
      }
    }
    else if (vsid == 0xff01) {
      if (vdm_type == PD_VDM_USBPD_DISCOVER_MODES) {
        printf("# [pd]   vdm: ACK DP discover_modes\n");

        uint32_t portcap = obj1 & 0b11;
        uint32_t dp_signals = (obj1 & (0b1111 << 2)) >> 2;
        uint32_t recept_ind = !!(obj1 & (1 << 6)); // receptacle
        uint32_t usb2_not_used = !!(obj1 & (1 << 7)); // USB2 not required
        uint32_t dfp_pins = (obj1 & 0xff00) >> 8; // 0b00001000 << 8; // only "D" pin assignment (DP+USB mix)
        uint32_t ufp_pins = (obj1 & 0xff0000) >> 16;

        printf("# [pd]   DP mode 1:\n");
        printf("# [pd]     portcap: %02b\n", (unsigned int)portcap);
        printf("# [pd]     dp_signals: %04b\n", (unsigned int)dp_signals);
        printf("# [pd]     receptacle: %01b\n", (unsigned int)recept_ind);
        printf("# [pd]     usb2_not_used: %01b\n", (unsigned int)usb2_not_used);
        printf("# [pd]     dfp_pins: %08b\n", (unsigned int)dfp_pins);
        printf("# [pd]     ufp_pins: %08b\n", (unsigned int)ufp_pins);

        /*
          # [pd] <DP mode 1:
          # [pd]   portcap: 01
          # [pd]   dp_signals: 0011
          # [pd]   receptactle: 1
          # [pd]   usb2_not_used: 0
          # [pd]   dfp_pins: 00000000
          # [pd]   ufp_pins: 00011100 // <-- C,D,E assignments. why @ ufp?
         */

        /*
          xreal glasses:

          # [pd] <DP mode 1:
          # [pd]   portcap: 01
          # [pd]   dp_signals: 0001
          # [pd]   receptacle: 0
          # [pd]   usb2_not_used: 0
          # [pd]   dfp_pins: 00000100 // <-- E assignment
          # [pd]   ufp_pins: 00000000
         */

        portcap = 0b10; // we are DFP_D capable (DP source)
        // TODO: select the correct DP signal version. can we do DP2?
        dp_signals = 0b0001 << 2; // DP v1.3
        recept_ind = 0b1 << 6; // receptacle
        usb2_not_used = 0b0 << 7; // USB2 may be required

        // TODO error out when neither D nor E supported
        if (dfp_pins & PD_ALT_MODE_PINS_D) {
          // dp+usb3 mix
          alt_mode_pin_assignment = PD_ALT_MODE_PINS_D;
        } else {
          // 4 lanes dp
          alt_mode_pin_assignment = PD_ALT_MODE_PINS_E;
        }
        printf("# [pd]   alt mode pin assignment: %x\n", alt_mode_pin_assignment);
        dfp_pins = alt_mode_pin_assignment << 8;
        ufp_pins = alt_mode_pin_assignment << 16;

        uint32_t mode_flags = portcap|dp_signals|recept_ind|usb2_not_used|dfp_pins|ufp_pins;

        // enter mode
        // TODO: actually select the right mode, and not just choose the first one
        send_vdm2(0xff018104, mode_flags);
      }
      else if (vdm_type == PD_VDM_USBPD_ENTER_MODE) {
        printf("# [pd]   vdm: ACK DP enter_mode\n");

        // send status update
        uint32_t connected = 0b01; // "DFP_D is connected"
        uint32_t power_low = 0b0 << 2; // not set by DFP
        uint32_t enabled = 0b0 << 3; // not set by DFP
        uint32_t multi_fn_pref = 0b0 << 4; // not set by DFP

        uint32_t usb_req = 0b0 << 5; // not set by DFP
        uint32_t exit_dp = 0b0 << 6; // not set by DFP
        uint32_t hpd = 0b0 << 7; // not set by DFP
        send_vdm2(0xff018110, connected|power_low|enabled|multi_fn_pref|
                              usb_req|exit_dp|hpd);
      }
      else if (vdm_type == PD_VDM_USBPD_DP_STATUS_UPDATE) {
        printf("# [pd]   vdm: ACK DP status_update\n");

        // send configure message
        uint32_t select = 0b10; // ufp_u is ufp_d
        uint32_t signaling = 0b0001 << 2; // DP1.3
        uint32_t pin_assignment = alt_mode_pin_assignment << 8;
        send_vdm2(0xff018111, select|signaling|pin_assignment);

        int hpd = !!((obj1 & (1<<7)));
        printf("# [pd]   vdm: DP status_update, HPD: %d\n", hpd);
        if (mb_version() >= 2) {
          printf("# [pd]   [displayport] set HPD = %d\n", hpd);
          gpio_put(PIN_V20_DP_HPD, hpd);
        }
      }
      else if (vdm_type == PD_VDM_USBPD_DP_CONFIGURE) {
        printf("# [pd]   vdm: ACK DP configure\n");
      }
    }
  }

  // TODO error handling?
  return true;
}

static bool pd_handle_vdm_request() {
  printf("# [pd] pd_handle_vdm_request\n");
  uint32_t vdm_header = rx_msg.obj[0];
  uint16_t vsid = (vdm_header >> PD_VSID__SHIFT) & 0xffff;

  switch (vsid) {
  case PD_VSID_USBPD: {
    // USB-PD Standard "Vendor" ID
    switch (vdm_header & 0x1f) {
    case PD_VDM_USBPD_DISCOVER_IDENTITY:
      printf("# [pd]   replying to Discover Identity\n");
      tx.hdr = PD_MSGTYPE_D_VENDOR_DEFINED | PD_NUMOBJ(4) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
      tx.hdr &= ~PD_HDR_MESSAGEID;
      tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;

      // VDM Header
      tx.obj[0] = (PD_VSID_USBPD << PD_VSID__SHIFT) | PD_VDM_HEADER_STRUCTURED | PD_VDM_HEADER_TYPE_ACK | PD_VDM_USBPD_DISCOVER_IDENTITY;
      // ID VDO: USB Host capable; Product Type (DFP) = 010; Connector Type = 10; plus VID
      // bit 26: modal operation
      tx.obj[1] = 0x81400000 | USB_VID_PIDCODES | (1<<26);
      // Certification stat VDO
      tx.obj[2] = 0;
      // Product VDO: v1.0; plus PID
      tx.obj[3] = 0x01000000 | USB_PID_MNT_POCKET_REFORM_SYSCTL_10;

      fusb_send_message(&tx);
      tx_id_count++;

      return true;
    case PD_VDM_USBPD_DISCOVER_SVIDS:
      printf("# [pd]   replying to Discover SVIDs\n");

      // only one VDO
      tx.hdr = PD_MSGTYPE_D_VENDOR_DEFINED | PD_NUMOBJ(2) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
      tx.hdr &= ~PD_HDR_MESSAGEID;
      tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;

      // VDM Header
      tx.obj[0] = (PD_VSID_USBPD << PD_VSID__SHIFT) | PD_VDM_HEADER_STRUCTURED | PD_VDM_HEADER_TYPE_ACK | PD_VDM_USBPD_DISCOVER_SVIDS;
      // ID VDO: USB Host capable; Product Type (DFP) = 010; Connector Type = 10; plus VID

      // VDM
      // for enter/exit:
      // 001 – 110 = Index into the list of Vendor Defined Objects (VDOs) to identify the desired Mode VDO.
      tx.obj[1] = 0xff01;
      tx.obj[2] = 0;
      tx.obj[3] = 0;

      fusb_send_message(&tx);
      tx_id_count++;
      return true;
    default:
      printf("# [pd]   [vdm] rejecting unknown vdm_header 0x%lx\n", vdm_header & 0x1f);
      pd_send_not_supported();
      return false;
    }
  }
  case 0xff01: {
    // DisplayPort
    switch (vdm_header & 0x1f) {
    case PD_VDM_USBPD_DISCOVER_MODES:
      printf("# [pd]   [displayport] replying to Discover Modes\n");

      // only one VDO
      tx.hdr = PD_MSGTYPE_D_VENDOR_DEFINED | PD_NUMOBJ(2) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
      tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
      // VDM Header (svid: displayport)
      tx.obj[0] = (0xff01 << PD_VSID__SHIFT) | PD_VDM_HEADER_STRUCTURED | PD_VDM_HEADER_TYPE_ACK | PD_VDM_USBPD_DISCOVER_MODES;

      uint32_t portcap = 0b10; // DFP_D capable
      uint32_t dp_signals = 0b0001 << 2; // DP v1.3
      uint32_t recept_ind = 0b1 << 6; // receptacle
      uint32_t usb2_not_used = 0b1 << 7; // USB2 not required
      uint32_t dfp_pins = alt_mode_pin_assignment << 8; // only "D" pin assignment (DP+USB mix)
      uint32_t ufp_pins = alt_mode_pin_assignment << 16;
      tx.obj[1] = portcap|dp_signals|recept_ind|usb2_not_used|dfp_pins|ufp_pins;

      tx.obj[2] = 0;
      tx.obj[3] = 0;
      fusb_send_message(&tx);
      tx_id_count++;
      return true;
    case PD_VDM_USBPD_ENTER_MODE:
      printf("# [pd]   [displayport] replying to Enter Mode\n");

      tx.hdr = PD_MSGTYPE_D_VENDOR_DEFINED | PD_NUMOBJ(1) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
      tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;

      // VDM Header (svid: displayport)
      tx.obj[0] = (0xff01 << PD_VSID__SHIFT) | PD_VDM_HEADER_STRUCTURED | PD_VDM_HEADER_TYPE_ACK | PD_VDM_USBPD_ENTER_MODE | (0x1 << 8); // TODO hack: 0x100 = mode 1
      tx.obj[1] = 0;
      tx.obj[2] = 0;
      tx.obj[3] = 0;
      fusb_send_message(&tx);
      tx_id_count++;
      return true;

    case PD_VDM_USBPD_DP_STATUS_UPDATE:
      int hpd_state = !!(rx_msg.obj[1] & 0x80);
      printf("# [pd]   [displayport] DP Status Update object 1: 0x%lx\n", rx_msg.obj[1]);
      printf("# [pd]   [displayport] DP Status Update HPD: %d\n", hpd_state);
      printf("# [pd]   [displayport] replying to DP Status Update\n");

      tx.hdr = PD_MSGTYPE_D_VENDOR_DEFINED | PD_NUMOBJ(2) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
      tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;

      // VDM Header (svid: displayport)
      tx.obj[0] = (0xff01 << PD_VSID__SHIFT) | PD_VDM_HEADER_STRUCTURED | PD_VDM_HEADER_TYPE_ACK | PD_VDM_USBPD_DP_STATUS_UPDATE | (0x100); // TODO hack: 0x100 = mode 1
      tx.obj[1] = 0b00000001; // FIXME undo while restoring
      //
      //tx.obj[1] = 0b00011011;
      //                  --  DFP_D is connected
      //             -----    "apply only to displayport status sent by a UFP_U to a DFP_U"
      //                      --> but we are DFP_U and DFP_D
      //            -         clear the HPD status bits
      tx.obj[2] = 0;
      tx.obj[3] = 0;
      fusb_send_message(&tx);
      tx_id_count++;

      if (mb_version() >= 2) {
        printf("# [pd]   [displayport] set HPD = %d\n", hpd_state);
        gpio_put(PIN_V20_DP_HPD, hpd_state);
      }
      return true;

    case PD_VDM_USBPD_DP_CONFIGURE:
      printf("# [pd]   [displayport] DP Configure object 1: 0x%lx\n", rx_msg.obj[1]);
      printf("# [pd]   [displayport] replying to DP Configure\n");

      tx.hdr = PD_MSGTYPE_D_VENDOR_DEFINED | PD_NUMOBJ(2) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
      tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
      // DFP_D = display source
      // VDM Header (svid: displayport)
      tx.obj[0] = (0xff01 << PD_VSID__SHIFT) | PD_VDM_HEADER_STRUCTURED | PD_VDM_HEADER_TYPE_ACK | PD_VDM_USBPD_DP_CONFIGURE | (0x100); // TODO hack: 0x100 = mode 1
      tx.obj[1] = 0b000110;
      //                -- UFP_U is UFP_D
      //            ----   DP 1.3
      tx.obj[1] |= alt_mode_pin_assignment << 8;
      tx.obj[2] = 0;
      tx.obj[3] = 0;
      fusb_send_message(&tx);
      tx_id_count++;
      return true;

    case PD_VDM_USBPD_ATTENTION:
      int hpd_state2 = !!(rx_msg.obj[1] & 0x80);
      printf("# [pd]   [displayport] replying to DP Attention (hpd = %d)\n", hpd_state2);
      send_vdm((0xff01 << PD_VSID__SHIFT) | PD_VDM_HEADER_STRUCTURED | PD_VDM_HEADER_TYPE_ACK | PD_VDM_USBPD_ATTENTION, 0);
      if (mb_version() >= 2) {
        printf("# [pd]   [displayport] set HPD = %d\n", hpd_state2);
        gpio_put(PIN_V20_DP_HPD, hpd_state2);
      }
      return true;

    default:
      printf("# [pd]   [displayport] rejecting unknown vdm_header 0x%lx\n", vdm_header & 0x1F);
      pd_send_not_supported();
      return false;
    }
  }
  default:
    printf("# [pd]   rejecting unknown vendor 0x%x\n", vsid);
    pd_send_not_supported();
    return false;
  }
  return false;
}

static void debug_status0(uint8_t status0) {
  if (status0 & FUSB_STATUS0_VBUSOK) {
    printf("# [pd]   status0 VBUSOK\n");
  }
  if (status0 & FUSB_STATUS0_ACTIVITY) {
    printf("# [pd]   status0 ACTIVITY\n");
  }
  if (status0 & FUSB_STATUS0_COMP) {
    printf("# [pd]   status0 COMP\n");
  }
  if (status0 & FUSB_STATUS0_CRC_CHK) {
    printf("# [pd]   status0 CRC_CHK\n");
  }
  if (status0 & FUSB_STATUS0_ALERT) {
    printf("# [pd]   status0 ALERT\n");
  }
  if (status0 & FUSB_STATUS0_WAKE) {
    printf("# [pd]   status0 WAKE\n");
  }
}

bool swap_dr_after_goodcrc = false;

static bool pd_handle_msg_from_power_source(union pd_msg *msg, [[maybe_unused]] battery_info_s* battery_info) {
  uint32_t msgtype = PD_MSGTYPE_GET(msg);
  uint32_t numobj = PD_NUMOBJ_GET(msg);
  uint32_t vdm_header = msg->obj[0];

  printf("# [pd] handle_msg_from_power_source: hdr: 0x%08x their datarole: %s our datarole: %s\n", msg->hdr, PD_DATAROLE_STR(msg), pd_datarole?"DFP":"UFP");
  uint16_t their_datarole = msg->hdr & PD_HDR_DATAROLE;

  if (msg->hdr & PD_HDR_EXT) {
    // extended messages.
    printf("# [pd]   rx extended message.\n");
    pd_send_not_supported();
    return false;
  }

  // control messages
  if (numobj == 0) {
    switch (msgtype) {
    case PD_MSGTYPE_C_GOODCRC:
      // TODO: we should care about these in some situations.
      printf("# [pd]   supply sent goodcrc.\n");
      if (swap_dr_after_goodcrc) {
        if (their_datarole == PD_DATAROLE_DFP) {
          pd_datarole = PD_DATAROLE_DFP;
          printf("# [pd]     switched our data role to DFP.\n");

          // TODO: only on mb2
          if (!alt_mode_requested) {
            printf("# [pd]     attempting DP Alt-Mode trigger...\n");
            send_vdm(0xff008002, 0);
            alt_mode_requested = true;
          }
        } else {
          pd_datarole = PD_DATAROLE_UFP;
          printf("# [pd]     switched our data role to UFP.\n");
        }
        swap_dr_after_goodcrc = false;
      }
      return true;

    case PD_MSGTYPE_C_ACCEPT:
      printf("# [pd]   supply accepted our requested PDO.\n");
      return true;

    case PD_MSGTYPE_C_PS_RDY:
      // power supply is ready
      printf("# [pd]   power supply ready.\n");
      charger_enable_charge(requested_current);
      return true;

    case PD_MSGTYPE_C_DR_SWAP:
      // other side wants to swap data role.
      printf("# [pd]   power supply wants to swap data role.\n");
      if (their_datarole == PD_DATAROLE_DFP) {
        if (!battery_info->som_is_powered) {
          // SOM is not powered, so it will not act as a host. Tell partner to try later.
          printf("# [pd]   replying with wait to data role swap request\n");
          tx.hdr = PD_MSGTYPE_C_WAIT | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
          tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
          fusb_send_message(&tx);
          tx_id_count++;
        } else {
          // Accept. We become the DFP (host).
          printf("# [pd]   accepting data role swap.\n");
          send_source_accept();
          swap_dr_after_goodcrc = true;
        }
      } else {
        printf("# [pd]   accepting data role swap.\n");
        send_source_accept();
        swap_dr_after_goodcrc = true;
      }
      return true;

    case PD_MSGTYPE_C_PR_SWAP:
      printf("# [pd]   power supply wants to swap power role.\n");
      printf("# [pd]   accepting power-role swap snk->src\n");
      send_source_accept();
      pd_powerrole = PD_POWERROLE_SOURCE;
      pd_state = PD_STATE_UNATTACHED_SRC;
      return true;

    case PD_MSGTYPE_C_GET_SINK_CAP:
      printf("# [pd]   responding to PD_MSGTYPE_C_GET_SINK_CAP\n");
      send_sink_cap(0);
      return true;

    case PD_MSGTYPE_C_VCONN_SWAP:
      printf("# [pd]   accepting PD_MSGTYPE_C_VCONN_SWAP\n");
      //pd_send_not_supported();
      enable_vconn = true;
      send_source_accept();

      // try triggering alt mode
      // TODO: weird place
      //send_vdm(0xff008002, 0);
      return true;
    case PD_MSGTYPE_C_SOFT_RESET:
      printf("# [pd]   accepting PD_MSGTYPE_C_SOFT_RESET\n");
      pd_send_reset();
      return false;
    case PD_MSGTYPE_C_WAIT:
      printf("# [pd]   accepting PD_MSGTYPE_C_WAIT\n");
      // TODO: check how to handle WAIT
      return false;

    default:
      printf("# [pd]   pd_handle_msg_from_power_source: rejecting unsupported control message\n");
      pd_send_not_supported();
      return false;
    }
  }
  else if (msgtype == PD_MSGTYPE_D_SOURCE_CAPABILITIES) {
    struct picked_pdo picked = pick_pdo(&rx_msg);
    // FIXME: what about headroom for passing power to other USB devices?
    requested_current = picked.max_current;
    if (requested_current > 300) {
      requested_current = 300;
    }
    if (requested_current < 10) {
      requested_current = 10;
    }

    printf("# [pd]   requesting PDO %u, %u V (max %u mA) at %u mA\n", picked.pdo_num, picked.voltage, picked.max_current * 10, requested_current * 10);

    tx.hdr = PD_MSGTYPE_D_REQUEST | PD_NUMOBJ(1) | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
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
  else if (msgtype == PD_MSGTYPE_D_REQUEST) {
    printf("# [pd]   rejecting PD_MSGTYPE_D_REQUEST\n");
    pd_send_not_supported();
    return false;
  }
  else if (msgtype == PD_MSGTYPE_D_VENDOR_DEFINED) {
    /*
      # [pd] handle_msg_from_power_source: hdr: 0x00001361 obj0: 0x2f01900a
      # [fusb] rxb 0xe0 msgtype 0x01 msgid 7 role SRC numobj 0 size 0
      # [pd] handle_msg_from_power_source: hdr: 0x00000f61 obj0: 0x2f01900a
     */
    // "Vendor"-specific data message
    uint16_t vsid = (vdm_header >> PD_VSID__SHIFT) & 0xffff;
    printf("# [pd]   handle_msg_from_power_source: vdm: 0x%lx vsid: %x\n", (unsigned long)vdm_header, vsid);

    if ((vdm_header & PD_VDM_HEADER_STRUCTURED) == 0) {
      // Unstructured message, which we cannot parse.
      printf("# [pd]   pd_handle_msg_from_power_source: rejecting PD_VDM_HEADER_STRUCTURED\n");
      pd_send_not_supported();
      return false;
    }

    // TOOD check
    /*if ((vdm_header & 0x00e0) != 0) {
      // non-REQ message, which we cannot parse. ignore it.
      printf("# [pd] pd_handle_msg_from_power_source: rejecting non-REQ message\n");
      return false;
      //return pd_handle_vdm_response();
    }*/

    if (mb_version() < 2) {
      printf("# [pd]   [vdm, mb v1.0] rejecting unsupported vdm_header 0x%lx\n", (unsigned long)vdm_header & 0x1f);
      pd_send_not_supported();
      return false;
    }

    // handle VDMs
    // TODO: handle BUSY
    if (vdm_header & PD_VDM_HEADER_TYPE_ACK) {
      printf("# [pd]   source sent VDM ACK\n");
      return pd_handle_vdm_response();
    } else if (vdm_header & PD_VDM_HEADER_TYPE_NAK) {
      printf("# [pd]   source sent VDM NAK\n");
      return false;
    } else {
      printf("# [pd]   source sent VDM request (?)\n");
      return pd_handle_vdm_request();
    }
  } else {
    // unsupported message type
    printf("# [pd]   rejecting unsupported PD_MSGTYPE_D: %02lx\n", msgtype);
    pd_send_not_supported();
    return false;
  }
}

static bool pd_handle_msg_from_power_sink(union pd_msg *msg, [[maybe_unused]] battery_info_s* battery_info) {
  uint32_t msgtype = PD_MSGTYPE_GET(msg);
  uint32_t numobj = PD_NUMOBJ_GET(msg);
  uint32_t vdm_header = msg->obj[0];

  printf("# [pd] handle_msg_from_power_sink: hdr: 0x%08x obj0: 0x%08lx\n", msg->hdr, (unsigned long)vdm_header);

  // numobj = 0 -> message type C
  // numobj > 0 -> message type D
  // 0x0041 = '0b0000 0000 0100 0001'
  //             |         |||`-````--- 5 msgtype_c/d bits (1 = goodcrc)
  //             |         ||`--------- datarole UFP
  //             |         ``---------- SPECREV. 01 = 2.0
  //             `--------------------- extended message (bit 15)
  // 0x1042 = '0b0001 0000 0100 0010'
  //              ```------------------ numobj (0-7)
  //
  // 0x51a1 = '0b0101 0001 1010 0001
  //                     | || `-````--- msgtype source capabilities
  //                     | ``---------- specrev 3
  //                     `------------- powerrole src
  // 0x0363 = '0b0000 0011 0110 0011
  //                       |||`-````--- msgtype ACCEPT
  //                       ||`--------- datarole DFP
  //                       ``---------- specrev 2
  //                  ```-------------- message id

  if (msg->hdr & PD_HDR_EXT) {
    // extended messages.
    printf("# [pd]   rx extended message.\n");
    pd_send_not_supported();
    return false;
  }

  if (numobj == 0) {
    // control message
    switch (msgtype) {
    case PD_MSGTYPE_C_GOODCRC:
      printf("# [pd]   [rx from sink] goodcrc.\n");
      pd_source_cap_acked = true;

      if (source_pdo_accept_sent && !source_pdo_ready_sent) {
        send_ps_ready();
        source_pdo_accept_sent = 0;
        source_pdo_ready_sent = 1;
        source_pdo_ready_acked = 0;
      } else if (source_pdo_ready_sent) {
        source_pdo_ready_acked = 1;
      }
      return true;
    case PD_MSGTYPE_C_GET_SOURCE_CAP:
      printf("# [pd]   [rx from sink] get_source_cap.\n");
      send_source_cap(0);
      return true;
    case PD_MSGTYPE_C_GET_SINK_CAP:
      printf("# [pd]   [rx from sink] get_sink_cap.\n");
      send_sink_cap(0);
      return true;
    case PD_MSGTYPE_C_WAIT:
      printf("# [pd]   [rx from sink] wait.\n");
      return false;
    case PD_MSGTYPE_C_SOFT_RESET:
      printf("# [pd]   [rx from sink] soft_reset.\n");
      pd_state = PD_STATE_SETUP;
      return false;
    case PD_MSGTYPE_C_ACCEPT:
      printf("# [pd]   [rx from sink] accept.\n");
      return true;
    case PD_MSGTYPE_C_GOTOMIN:
      printf("# [pd]   [rx from sink] gotomin.\n");
      return false;
    case PD_MSGTYPE_C_REJECT:
      printf("# [pd]   [rx from sink] reject.\n");
      return false;
    case PD_MSGTYPE_C_PING:
      printf("# [pd]   [rx from sink] ping.\n");
      pd_send_not_supported();
      return false;
    case PD_MSGTYPE_C_DR_SWAP:
      printf("# [pd]   [rx from sink] dr_swap.\n");
      pd_send_not_supported();
      return false;
    case PD_MSGTYPE_C_PR_SWAP:
      printf("# [pd]   [rx from sink] pr_swap.\n");
      send_source_accept();
      pd_powerrole = PD_POWERROLE_SINK;
      pd_state = PD_STATE_UNATTACHED_SNK;
      return true;
    case PD_MSGTYPE_C_VCONN_SWAP:
      printf("# [pd]   [rx from sink] vconn_swap.\n");
      enable_vconn = false;
      send_source_accept();
      return true;
    default:
      printf("# [pd]   [rx from sink] unknown msgtype: %d.\n", (int)msgtype);
      pd_send_not_supported();
      return false;
    }
    return false;
  } else {
    // data message
    switch (msgtype) {
    case PD_MSGTYPE_D_REQUEST: {
      int req_idx = PD_RDO_OBJPOS_GET(msg);
      printf("# [pd]   [rx from sink] request: PDO %d\n", req_idx);
      send_source_accept();
      source_pdo_accept_sent = 1;
      source_pdo_ready_sent = 0;
      return true;
    }
    default:
      // TODO: handle BUSY
      if (vdm_header & PD_VDM_HEADER_TYPE_ACK) {
        return pd_handle_vdm_response();
      } else if (vdm_header & PD_VDM_HEADER_TYPE_NAK) {
        printf("# [pd]   [rx from sink] VDM NAK\n");
        return false;
      } else {
        return pd_handle_vdm_request();
      }
    }
    pd_send_not_supported();
    return false;
  }
}

static int pd_src_attempts = 0;

// Returns if state was "changed" in some form and we expect to maybe be called again.
static bool pd_comm_pd(battery_info_s* battery_info) {
  uint8_t status1;
  if (fusb_read_buf(FUSB_STATUS1, 1, &status1)) {
    if (status1 & 0b11010111) {
      printf("# [pd] status1: 0b%08b\n", status1);
      if (status1 & 0b00100100) {
        // TODO: handle the right bit!
        printf("# [pd] ~~ tx flush / pd reset ~~\n");
        fusb_write_byte(FUSB_CONTROL0, (pd_host_current << FUSB_CONTROL0_HOST_CUR_SHIFT) | FUSB_CONTROL0_TX_FLUSH);
        fusb_write_byte(FUSB_RESET, FUSB_RESET_PD_RESET);
        if (pd_src_attempts++ > 4) {
          // give up retrying in the current state (probably PD_STATE_UNATTACHED_SRC)
          pd_state = PD_STATE_SETUP;
          pd_src_attempts = 0;
        }
        return true;
      }
    }
  }

  // FIXME: this does not enforce the proper message order. maybe ok as is, maybe not.
  if (!fusb_read_message(&rx_msg)) {
    return false;
  }

  unsigned int msgrole = PD_POWERROLE_GET(&rx_msg);
  if (msgrole == PD_POWERROLE_SOURCE) {
    return pd_handle_msg_from_power_source(&rx_msg, battery_info);
  } else {
    return pd_handle_msg_from_power_sink(&rx_msg, battery_info);
  }
}

bool pd_tick(battery_info_s* battery_info) {
  bool can_sleep = false;
  if (pd_state != PD_STATE_SETUP) {
    pd_set_fusb_switches();
    pd_comm_pd(battery_info);
  }

  if (pd_state == PD_STATE_SETUP) {
    printf("# [pd] PD_STATE_SETUP\n");

    // role defaults
    pd_powerrole = PD_POWERROLE_SINK;
    pd_datarole = PD_DATAROLE_UFP;
    pd_src_attempts = 0;
    swap_dr_after_goodcrc = false;
    pd_ccpin = 0;
    pd_host_current = 0b10;
    source_pdo_accept_sent = 0;
    source_pdo_ready_sent = 0;
    source_pdo_ready_acked = 0;
    pd_source_cap_acked = false;
    alt_mode_requested = false;
    enable_vconn = false;
    tx_id_count = 0;
    alt_mode_pin_assignment = PD_ALT_MODE_PINS_E;
    pd_set_fusb_switches();

    if (mb_version() >= 2) {
      printf("# [pd]   [displayport] set HPD = 0\n");
      gpio_put(PIN_V20_DP_HPD, 0);
    }

    // setup/timeout state
    if (battery_info->emergency_charge_necessary) {
      printf("# [pd] PD_STATE_SETUP - emergency_charge_necessary - not initializing PD\n");
      // TODO: don't read mps registers directly!
      if (mps_reg_config.config0.chg_en != 1) {
        // 500mA, should be safe and get us to at least a minimal charge.
        charger_enable_charge(50);
      }
      goto out;
    }
    charger_disable_charge();
    // TODO naming
    request_sent = 0;

    if (fusb_probe()) {
      // SW_RES: Reset the FUSB302B including the I2C registers to their default values
      if (!fusb_write_byte(FUSB_RESET, FUSB_RESET_SW_RES))
        goto out;

      busy_wait_us(100);

      // enable toggle and DRP mode
      int mode;
      if (battery_info->som_is_powered && !pd_force_sink) {
        mode = 1 << FUSB_CONTROL2_MODE_SHIFT;  // DRP
      } else {
        mode = 0b10 << FUSB_CONTROL2_MODE_SHIFT;  // SNK only
      }

      // unmask all interrupts to be able to wake from
      // dormant mode via USB-C events
      fusb_write_byte(FUSB_CONTROL0, (pd_host_current << FUSB_CONTROL0_HOST_CUR_SHIFT) | FUSB_CONTROL0_TX_FLUSH);
      fusb_write_byte(FUSB_CONTROL1, 0);
      // TODO: when oring FUSB_CONTROL1_ENSOP1 | FUSB_CONTROL1_ENSOP2
      // to FUSB_CONTROL1, we can see sop prime messages, but our code can't handle them yet.

      if (!fusb_write_byte(FUSB_CONTROL2, FUSB_CONTROL2_TOGGLE | mode | FUSB_CONTROL2_TOG_RD_ONLY))
        goto out;

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
    //printf("irqa: 0x%08x\n", i_irqa);

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
      // TODO: nothing else on CONTROL2?
      fusb_write_byte(FUSB_CONTROL2, 0);

      if (pd_state == PD_STATE_UNATTACHED_SNK) {
        pd_powerrole = PD_POWERROLE_SINK;
        pd_datarole = PD_DATAROLE_UFP;  // default for powerrole SINK
      } else {
        pd_powerrole = PD_POWERROLE_SOURCE;
        pd_datarole = PD_DATAROLE_DFP;  // default for powerrole SOURCE
      }

      pd_set_fusb_switches();
      // Enable all FUSB blocks, including PD BMC and measure block.
      fusb_write_byte(FUSB_POWER, 0xf);
      t = 0;
    }
  } else if (pd_state == PD_STATE_UNATTACHED_SNK) {
    // unattached.snk. Wait for VBUS to arrive.
    uint8_t status0;
    if (fusb_read_buf(FUSB_STATUS0, 1, &status0)) {
      if (status0 & FUSB_STATUS0_VBUSOK) {
        printf("# [pd] PD_STATE_UNATTACHED_SNK VBUS is now OK\n");
        t = 0;
        pd_state = PD_STATE_ATTACHED_SNK;
      }
    }
    if (pd_state == PD_STATE_UNATTACHED_SNK && t > 2000) {
      // timeout
      // FIXME: timeout value?
      printf("# [pd] PD_STATE_UNATTACHED_SNK - timeout waiting for VBUS\n");
      t = 0;
      pd_state = PD_STATE_SETUP;
    }
  } else if (pd_state == PD_STATE_ATTACHED_SNK) {
    // attached.snk.
    // need to handshake charging capability and wait for ps_ok
    // process PD communication

    // detect detach by VBUS going away.
    uint8_t status0;
    if (fusb_read_buf(FUSB_STATUS0, 1, &status0) && (status0 & FUSB_STATUS0_VBUSOK) == 0) {
      printf("# [pd] PD_STATE_ATTACHED_SNK VBUS went away\n");
      t = 0;
      pd_state = PD_STATE_SETUP;
      goto out;
    } else {
      if (t>10000 && !mps_reg_config.config0.chg_en) {
        // for some reason charging did not start.
        // TODO: send soft reset first.
        // TODO: fix timer.
        // TODO: don't read mps registers directly!
        printf("# [pd] PD_STATE_ATTACHED_SNK timeout while handshaking, reset\n");
        t = 0;
        pd_state = PD_STATE_SETUP;
      } else if (t>8000 && !mps_reg_config.config0.chg_en && !pd_sent_soft_reset) {
        // TODO: don't read mps registers directly!
        // Charging did not start.
        // This situation was observed with an Apple 30W charger, which apparently ignores a hard-reset
        // without a soft-reset and without an actual detach. Unclear why this happens.
        // Necessary to handle this so charging resumes after sysctl gets rebooted by a firmware upgrade.
        // TODO: the usbpd_sent_soft_reset stuff is not great.
        // TODO: fix timer.
        pd_sent_soft_reset = true;
        printf("# [pd] PD_STATE_ATTACHED_SNK timeout while handshaking, sending soft reset\n");
        tx.hdr = PD_MSGTYPE_C_SOFT_RESET | pd_datarole | (pd_powerrole << PD_HDR_POWERROLE_SHIFT) | PD_VERSION;
        tx.hdr |= (tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;
        fusb_send_message(&tx);
        tx_id_count++;
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
    if (t == 1) {
      printf("# [pd] PD_STATE_UNATTACHED_SRC\n");
    }

    // club3d only works with 100, not with 50, not with 200
    if (t >= 10) {
      uint8_t status0;
      if (fusb_read_buf(FUSB_STATUS0, 1, &status0)) {
        [[maybe_unused]] uint8_t comp = status0 & FUSB_STATUS0_COMP;
        [[maybe_unused]] uint8_t bc_lvl = status0 & FUSB_STATUS0_BC_LVL;
        printf("# [pd] PD_STATE_UNATTACHED_SRC, status0 = %02x bc_lvl = %02x\n", status0, bc_lvl);
        debug_status0(status0);
        // COMP relies on correct MDAC setting
        if (comp == 0) {
          pd_state = PD_STATE_ATTACHED_SRC;
          pd_source_cap_acked = false;
          t = 0;
        } else {
          // timeout after 1 sec
          if (t >= 1000) {
            pd_state = PD_STATE_SETUP;
            t = 0;
          }
        }
      }
    }

    if (t >= 10000) {
      printf("# [pd] PD_STATE_UNATTACHED_SRC fusb_read timeout\n");
      pd_state = PD_STATE_SETUP;
      t = 0;
    }
  } else if (pd_state == PD_STATE_ATTACHED_SRC) {
    // the spec says: tTypeCSendSourceCap min 100ms, max 200ms, nom 150ms!
    // 30 is a magic number that works for club3d, xreal, apple hdmi.
    // TODO: replace with real ms timer
    if (t % 30 == 0) {
      if (!pd_source_cap_acked) {
        // every 100ms, try sending a burst of source caps
        tx_id_count = 0;
        send_source_cap(0);
      }
    }

    if ((t % 1000 == 0) && pd_source_cap_acked && !source_pdo_ready_sent) {
      // we sent source caps, they were acked but nothing else happened, reset
      pd_send_reset();
      goto out;
    }

    if (source_pdo_ready_acked && !alt_mode_requested) {
      // TODO WIP
      // works for xreal, club3d, apple hdmi
      printf("# [pd] PD_STATE_ATTACHED_SRC: trying to trigger DP alt-mode...\n");
      send_vdm(0xff008002, 0);
      alt_mode_requested = true;
    }

    // detect detach
    if (t % 500 == 0) {
      uint8_t status0 = 0xff;
      fusb_read_buf(FUSB_STATUS0, 1, &status0);
      [[maybe_unused]] uint8_t comp = status0 & FUSB_STATUS0_COMP;
      [[maybe_unused]] uint8_t bc_lvl = status0 & FUSB_STATUS0_BC_LVL;
      printf("# [pd] PD_STATE_ATTACHED_SRC: status0 = 0x%02x (comp %d bc_lvl %d)\n", status0, comp, bc_lvl);
      debug_status0(status0);

      // COMP relies on correct MDAC setting
      if (comp) {
        pd_state = PD_STATE_SETUP;
      }
    }
  }

 out:
  t++;
  return can_sleep;
}
