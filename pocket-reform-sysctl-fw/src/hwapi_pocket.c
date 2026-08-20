/*
  MNT Pocket Reform: Hardware Interface
 */

#include "cli.h"
// TODO: move battery_info_s to pocket specific file?
#include "ext_gpio.h"
#include "hardware/timer.h"
#include "sysctl.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static battery_info_s *battery_info;

#define LEGACY_BUF_SZ 128
#define LEGACY_SPI_SZ 8
static char legacy_buf[LEGACY_BUF_SZ];
static char legacy_spi_buf[LEGACY_SPI_SZ];

uint64_t hwapi_legacy_q() {
  // execute status query command
  memset(legacy_spi_buf, 0, LEGACY_SPI_SZ);
  uint8_t percentage = (uint8_t)battery_info->charge_percentage;
  int16_t volts_int = (int16_t)(battery_info->battery_volts * 1000.0);
  int16_t current_int = (int16_t)(battery_info->battery_amps * 1000.0);

  legacy_spi_buf[0] = (uint8_t)volts_int;
  legacy_spi_buf[1] = (uint8_t)(volts_int >> 8);
  legacy_spi_buf[2] = (uint8_t)current_int;
  legacy_spi_buf[3] = (uint8_t)(current_int >> 8);
  legacy_spi_buf[4] = (uint8_t)percentage;
  legacy_spi_buf[5] = (uint8_t)0;
  return eightcc(legacy_spi_buf);
}

uint64_t hwapi_legacy_v() {
  // get cell voltage
  memset(legacy_buf, 0, LEGACY_SPI_SZ);
  // pack 0
  int volts = battery_info->cell1_volts;
  legacy_spi_buf[0] = (uint8_t)volts;
  legacy_spi_buf[1] = (uint8_t)(volts >> 8);
  volts = battery_info->cell2_volts;
  legacy_spi_buf[2] = (uint8_t)volts;
  legacy_spi_buf[3] = (uint8_t)(volts >> 8);
  return eightcc(legacy_spi_buf);
}

uint64_t hwapi_legacy_c() {
  // get calculated capacity (emulated)
  memset(legacy_buf, 0, LEGACY_SPI_SZ);
  uint16_t cap_accu = (uint16_t)BATTERY_CAPACITY_MILLIAMP_HOURS * (((float)battery_info->charge_percentage) / 100.0);
  uint16_t cap_min = (uint16_t)0;
  uint16_t cap_max = (uint16_t)BATTERY_CAPACITY_MILLIAMP_HOURS;
  legacy_spi_buf[0] = (uint8_t)cap_accu;
  legacy_spi_buf[1] = (uint8_t)(cap_accu >> 8);
  legacy_spi_buf[2] = (uint8_t)cap_min;
  legacy_spi_buf[3] = (uint8_t)(cap_min >> 8);
  legacy_spi_buf[4] = (uint8_t)cap_max;
  legacy_spi_buf[5] = (uint8_t)(cap_max >> 8);
  return eightcc(legacy_spi_buf);
}

char* hwapi_legacy_kbd_s() {
  return (char*)"MNT Pocket Reform SC" MNTRE_FIRMWARE_VERSION "\r\n";
}

char* hwapi_legacy_kbd_c() {
  int ma = (int)(battery_info->battery_amps * 1000.0);
  char ma_sign = ' ';
  if (ma < 0) {
    ma = -ma;
    ma_sign = '-';
  }
  int mv = (int)(battery_info->battery_volts * 1000.0);
  snprintf(legacy_buf, 128,
           "%02d %02d %02d %02d %02d %02d %02d %02d mA%c%04dmV%05d %3d%% P%d\r\n",
           (int)(battery_info->cell1_volts / 100),
           (int)(battery_info->cell2_volts / 100), (int)(0), (int)(0),
           (int)(0), (int)(0), (int)(0), (int)(0), ma_sign, ma, mv,
           battery_info->charge_percentage,
           battery_info->som_is_powered ? 1 : 0);

  legacy_buf[127] = 0;
  return (char*)legacy_buf;
}

uint64_t hwapi_set_backlight([[maybe_unused]] struct cli_context* ctx, uint64_t brightness) {
  // only for display v2
  // 70% is a limit of the hardware (above, the backlight can flicker. some displays can go higher)
  if (brightness > 100) brightness = 100;
  // rescale 0..100% to 0..70% with a nice curve that has more detail/steps in the dark area
  double scaled = 0.72 + 23.1 * tan(0.0125 * ((double)brightness));
  // FIXME clip at 63% due to a bunch of sensitive panels
  if (scaled > 63.0) scaled = 63.0;

  // it could be that sysctl fw was updated without a reboot.
  // if there is a driver running that sends this command,
  // assume that we should enable disp v2 backlight control.
  set_display_v2_backlight_unlock(1);

  set_display_backlight((int)scaled);
  return brightness;
}

uint64_t hwapi_set_backlight_freq([[maybe_unused]] struct cli_context* ctx, uint64_t freq) {
  set_display_backlight_freq((int)freq);
  return freq;
}

uint64_t hwapi_set_rail([[maybe_unused]] struct cli_context* ctx, uint64_t rail, uint64_t state) {
  if (rail == 0) {
    if (state == 0) {
      turn_som_power_off();
    } else {
      turn_som_power_on();
    }
  }
  return state;
}

uint64_t hwapi_set_gpio([[maybe_unused]] struct cli_context* ctx, uint64_t id, uint64_t high) {
  switch (id) {
  case 0: {
    // 0: Display Panel Reset (active low)
    if (mb_version() < 2) {
      gpio_put(PIN_DISP_RESET, high);
      gpio_put(PIN_PWREN_LATCH, 1);
      gpio_put(PIN_PWREN_LATCH, 0);
    }
    if (mb_version() >= 2) {
      if (high) {
        gpio_ext_enable(GPIO_EXT_DISP_RESET_N);
      } else {
        gpio_ext_disable(GPIO_EXT_DISP_RESET_N);
      }
    }
    if (get_display_v2_backlight_unlocked()) {
      // is sysctl controlling the backlight?
      // then turn it off during reset (display sleeping)
      if (!high) {
        hwapi_set_backlight(ctx, 0);
      }
      // it will be back turned on by gpio 8.
      // we lock brightness control now, otherwise
      // barebox might stay dark after
      // a warm reboot when it issues a reset
      // but no backlight setting
      set_display_v2_backlight_unlock(0);
    }
    break;
  }
  case 1: {
    if (high) {
      gpio_ext_enable(GPIO_EXT_HUB_PWR_EN);
    } else {
      gpio_ext_disable(GPIO_EXT_HUB_PWR_EN);
    }
    break;
  }
  case 2: {
    if (high) {
      gpio_ext_enable(GPIO_EXT_PCIE_PWR_EN);
    } else {
      gpio_ext_disable(GPIO_EXT_PCIE_PWR_EN);
    }
    break;
  }
  case 3: {
    if (high) {
      gpio_ext_enable(GPIO_EXT_3V3_EN);
    } else {
      gpio_ext_disable(GPIO_EXT_3V3_EN);
    }
    break;
  }
  case 4: {
    if (high) {
      gpio_ext_enable(GPIO_EXT_USWITCH_OFF);
    } else {
      gpio_ext_disable(GPIO_EXT_USWITCH_OFF);
    }
    break;
  }
  case 5: {
    if (high) {
      gpio_ext_enable(GPIO_EXT_DISP_BL_PWR_EN);
    } else {
      gpio_ext_disable(GPIO_EXT_DISP_BL_PWR_EN);
    }
    break;
  }
  case 6: {
    // TODO: dangerous to directly control?
    charger_disable_charge();
    usb_host_5v_set(0, high);
    break;
  }
  case 7: {
    usb_host_5v_set(1, high);
    break;
  }
  case 8: {
    set_display_v2_backlight_unlock(high);
    if (high) {
      // set initial brightness after first unlock, will be
      // scaled by hwapi_set_backlight
      hwapi_set_backlight(ctx, 100);
    }
    break;
  }
  case 9: {
    if (mb_version() < 2) {
      gpio_put(PIN_DISP_EN, high);
      gpio_put(PIN_PWREN_LATCH, 1);
      gpio_put(PIN_PWREN_LATCH, 0);
    }
    if (mb_version() >= 2) {
      if (high) {
        gpio_ext_enable(GPIO_EXT_DISP_1EN_2BL);
      } else {
        gpio_ext_disable(GPIO_EXT_DISP_1EN_2BL);
      }
    }
    break;
  }
  }
  return high;
}

uint64_t hwapi_set_usb_mode([[maybe_unused]] struct cli_context* ctx, uint64_t port, uint64_t mode) {
  // toggle USB muxing modes
  set_usb_mode(port, mode);
  return 1;
}

uint64_t hwapi_set_usb_ports_sysctl([[maybe_unused]] struct cli_context* ctx) {
  // expose Sysctl on port 2, and SoC UART on port 1 (charging port)
  hwapi_set_usb_mode(ctx, 0, 0);
  hwapi_set_usb_mode(ctx, 1, 0);
  return 1;
}

uint64_t hwapi_set_usb_ports_host([[maybe_unused]] struct cli_context* ctx) {
  // set all ports to host mode
  hwapi_set_usb_mode(ctx, 0, 1);
  hwapi_set_usb_mode(ctx, 1, 1);
  return 1;
}

uint64_t hwapi_set_usb_ports_edl([[maybe_unused]] struct cli_context* ctx) {
  // expose EDL mode on port 2, and SoC UART on port 1 (charging port)
  hwapi_set_usb_mode(ctx, 0, 0);
  hwapi_set_usb_mode(ctx, 1, 2);
  return 1;
}

uint64_t hwapi_soc_wake([[maybe_unused]] struct cli_context* ctx) {
  som_wake();
  return 1;
}

/* prepare SoC suspend by turning off unneeded power sources */
uint64_t hwapi_soc_pre_suspend([[maybe_unused]] struct cli_context* ctx) {
  gpio_ext_disable(GPIO_EXT_HUB_PWR_EN);
  gpio_ext_disable(GPIO_EXT_3V3_EN);
  gpio_ext_disable(GPIO_EXT_DISP_BL_PWR_EN);
  // TODO mb1 support!
  return 1;
}

uint64_t hwapi_soc_post_suspend([[maybe_unused]] struct cli_context* ctx) {
  gpio_ext_enable(GPIO_EXT_3V3_EN);
  gpio_ext_enable(GPIO_EXT_HUB_PWR_EN);
  gpio_ext_enable(GPIO_EXT_DISP_BL_PWR_EN);
  // TODO mb1 support!
  return 1;
}

uint64_t hwapi_get_cell_mv([[maybe_unused]] struct cli_context* ctx, uint64_t cell_id) {
  if (cell_id == 1) return battery_info->cell1_volts;
  // TODO misnomer, actually mV
  return battery_info->cell1_volts;
}

uint64_t hwapi_get_pack_mv([[maybe_unused]] struct cli_context* ctx /*uint64_t pack_id*/) {
  // TODO what about pack_volts?
  return battery_info->battery_volts * 1000;
}

uint64_t hwapi_get_pack_ma([[maybe_unused]] struct cli_context* ctx /*uint64_t pack_id*/) {
  return battery_info->battery_amps * 1000;
}

uint64_t hwapi_get_pack_charge([[maybe_unused]] struct cli_context* ctx /*uint64_t pack_id*/) {
  return battery_info->charge_percentage;
}

uint64_t hwapi_get_cell_max_mah([[maybe_unused]] struct cli_context* ctx, [[maybe_unused]] uint64_t cell_id) {
  return battery_info->cell_max_mah;
}

uint64_t hwapi_get_sys_mv([[maybe_unused]] struct cli_context* ctx) {
  // TODO
  return battery_info->input_volts;
}

uint64_t hwapi_get_sys_ma([[maybe_unused]] struct cli_context* ctx) {
  // TODO
  // also: input amps?
  return 0;
}

uint64_t hwapi_get_wdog_scratch([[maybe_unused]] struct cli_context* ctx, uint64_t idx) {
  if (idx > 8) {
    return 0;
  }
  return (uint64_t)watchdog_hw->scratch[idx];
}

uint64_t hwapi_set_pack_debug([[maybe_unused]] struct cli_context* ctx, uint64_t on) {
  battery_info->print_pack_info = on;
  return on;
}

void hwapi_vdm([[maybe_unused]] struct cli_context* ctx, uint64_t message_type, uint64_t prime) {
  send_vdm(message_type, prime);
}

void hwapi_vdm2([[maybe_unused]] struct cli_context* ctx, uint64_t obj0, uint64_t obj1) {
  send_vdm2(obj0, obj1);
}

void hwapi_pd_cap([[maybe_unused]] struct cli_context* ctx, uint64_t prime) {
  send_source_cap(prime);
}

void hwapi_pd_send_reset([[maybe_unused]] struct cli_context* ctx) {
  pd_send_reset();
}

void hwapi_pd_set_max_voltage([[maybe_unused]] struct cli_context* ctx, uint64_t v) {
  pd_set_max_voltage_req(v);
}

void hwapi_pd_set_force_sink([[maybe_unused]] struct cli_context* ctx, uint64_t force) {
  pd_set_force_sink(!!force);
}

void hwapi_pocket_init(battery_info_s *binfo) {
  battery_info = binfo;

  /* register all available functions */
  cli_add_func("set-rail", hwapi_set_rail, 2, CLI_TYPE_UINT64);
  cli_add_func("set-gpio", hwapi_set_gpio, 2, CLI_TYPE_UINT64);
  if (mb_version() >= 2) {
    cli_add_func("set-usb\0", hwapi_set_usb_mode, 2, CLI_TYPE_UINT64);
    cli_add_func("usb-sc\0\0", hwapi_set_usb_ports_sysctl, 0, CLI_TYPE_UINT64);
    cli_add_func("usb-edl\0", hwapi_set_usb_ports_edl, 0, CLI_TYPE_UINT64);
    cli_add_func("usb-host", hwapi_set_usb_ports_host, 0, CLI_TYPE_UINT64);
  }
  cli_add_func("set-lite", hwapi_set_backlight, 1, CLI_TYPE_UINT64);
  cli_add_func("set-lfrq", hwapi_set_backlight_freq, 1, CLI_TYPE_UINT64);
  cli_add_func("cell-mv\0", hwapi_get_cell_mv, 1, CLI_TYPE_UINT64);
  cli_add_func("cell-mah", hwapi_get_cell_max_mah, 1, CLI_TYPE_UINT64);
  cli_add_func("pack-mv\0", hwapi_get_pack_mv, 1, CLI_TYPE_UINT64);
  cli_add_func("pack-ma\0", hwapi_get_pack_ma, 1, CLI_TYPE_UINT64);
  cli_add_func("pack-crg", hwapi_get_pack_charge, 1, CLI_TYPE_UINT64);
  cli_add_func("pack-dbg", hwapi_set_pack_debug, 1, CLI_TYPE_UINT64);
  cli_add_func("sys-mv\0\0", hwapi_get_sys_mv, 0, CLI_TYPE_UINT64);
  cli_add_func("sys-ma\0\0", hwapi_get_sys_ma, 0, CLI_TYPE_UINT64);
  cli_add_func("soc-wake", hwapi_soc_wake, 0, CLI_TYPE_UINT64);
  cli_add_func("soc-susp", hwapi_soc_pre_suspend, 0, CLI_TYPE_UINT64);
  cli_add_func("soc-psus", hwapi_soc_post_suspend, 0, CLI_TYPE_UINT64);
  cli_add_func("pwrsave\0", enter_powersave, 0, CLI_TYPE_VOID);
  cli_add_func("vdm\0\0\0\0\0", hwapi_vdm, 2, CLI_TYPE_VOID);
  cli_add_func("vdm2\0\0\0\0", hwapi_vdm2, 2, CLI_TYPE_VOID);
  cli_add_func("pdcap\0\0\0", hwapi_pd_cap, 1, CLI_TYPE_VOID);
  cli_add_func("pdreset\0", hwapi_pd_send_reset, 1, CLI_TYPE_VOID);
  cli_add_func("pdvolt\0\0", hwapi_pd_set_max_voltage, 1, CLI_TYPE_VOID);
  cli_add_func("pdsink\0\0", hwapi_pd_set_force_sink, 1, CLI_TYPE_VOID);
  cli_add_func("wdog-scr\0\0", hwapi_get_wdog_scratch, 2, CLI_TYPE_UINT64);

  // TODO: DP/altmode_set configs
  // altmode_set(0b110);
  // altmode_set(0b111);
  // altmode_set(0b100);
  // altmode_set(0b101);
  // altmode_set(0b000);
  // altmode_set(0b010);
  // gpio_set_dir(PIN_USB_LOADER_SW, GPIO_OUT);
  // gpio_put(PIN_USB_LOADER_SW, 1);
  // PIN_V20_DP_HPD z/1/0
  // serial fowarding (next)

  /* register constants */
  cli_add_word("mb-ver\0\0", mb_version());
  cli_add_word("pack-cnt", 1);
  cli_add_word("cell-cnt", 2);

  /* legacy API commands */
  cli_add_func("0p\0\0\0\0\0\0", turn_som_power_off, 0, CLI_TYPE_VOID);
  cli_add_func("1p\0\0\0\0\0\0", turn_som_power_on, 0, CLI_TYPE_VOID);
  cli_add_func("0q\0\0\0\0\0\0\0", hwapi_legacy_q, 0, CLI_TYPE_UINT64);
  cli_add_func("0v\0\0\0\0\0\0\0", hwapi_legacy_v, 0, CLI_TYPE_UINT64);
  cli_add_func("0c\0\0\0\0\0\0\0", hwapi_legacy_c, 0, CLI_TYPE_UINT64);
  cli_add_func("1w\0\0\0\0\0\0\0", som_wake, 0, CLI_TYPE_VOID);
  cli_add_word("0f\0\0\0\0\0\0\0", eightcc("MNT PRSC"));
  cli_add_word("1f\0\0\0\0\0\0\0", eightcc("20260801"));
  cli_add_word("2f\0\0\0\0\0\0\0", eightcc("00000000"));
  cli_add_func("s\0\0\0\0\0\0\0", hwapi_legacy_kbd_s, 0, CLI_TYPE_STR128);
  cli_add_func("c\0\0\0\0\0\0\0", hwapi_legacy_kbd_c, 0, CLI_TYPE_STR128);
}
