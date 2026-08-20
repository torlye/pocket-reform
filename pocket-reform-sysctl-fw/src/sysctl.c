/*
  SPDX-License-Identifier: GPL-3.0-or-later
  MNT Pocket Reform System Controller Firmware for RP2040
  Copyright 2023-2026 MNT Research GmbH

  fusb_read/write functions based on:
  https://git.clarahobbs.com/pd-buddy/pd-buddy-firmware/src/branch/master/lib/src/fusb302b.c
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sysctl.h"
#include "hardware/gpio.h"
#include "hardware/structs/watchdog.h"
#include "pico/divider.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/xosc.h"
#include "tusb.h"
#include "reform_stdio_usb.h"
//#include "altmode.h"
//#include "kvstore.h"
#include "cli.h"
#include "spi_com.h"
#include "uart_com.h"
#include "hwapi_pocket.h"
#include "hardware/rosc.h"

static uint8_t mps_fault_last = 0;
static battery_info_s battery_info = {0};

// The Pico boot rom uses watchdog scratch registers 0, 1, 4, 5, 6, and 7.
// That leaves 2 and 3 for our "system is on" magic.
// A _real_ power-on reset clears these registers, so if our magic is left over
// then we have either been updated while the system is on, or have run into an
// event with probability 2**-64.
bool syscon_warm_boot() {
  return (watchdog_hw->scratch[2] == BOOT_MAGIC_2 &&
          watchdog_hw->scratch[3] == BOOT_MAGIC_3);
}

void set_boot_magic() {
  watchdog_hw->scratch[2] = BOOT_MAGIC_2;
  watchdog_hw->scratch[3] = BOOT_MAGIC_3;
}

void clear_boot_magic() {
  watchdog_hw->scratch[2] = BOOT_MAGIC_OFF;
  watchdog_hw->scratch[3] = BOOT_MAGIC_OFF;
}

// copied from Pranjal Chanda, "RP2040 PWM Frequency and Duty cycle set algorithm"
// called from timer interrupt, no sleep allowed here!
/**
 *  @brief Set frequency and duty cycle for any PWM slice and channel
 *  @param[in] slice_num  The slice number the GPIO is associated to
 *  @param[in] chan       The channel number the GPIO is associated to
 *  @param[in] freq       The required frequency to be set
 *  @param[in] duty_cycle The required duty cycle in percentage 1->100
 *
 *  @return 1: Success; <0: Error
 */
int32_t pwm_set_freq_duty(uint32_t slice_num, uint32_t chan, uint32_t freq, int duty_cycle) {
  uint8_t clk_divider = 0;
  uint32_t wrap = 0;
  uint32_t clock = clock_get_hz(clk_sys);

  if (freq < 8 || freq > clock) {
    /* This is the frequency range of generating a PWM
       in RP2040 at 125MHz */
    return -1;
  }

  for (clk_divider = 1; clk_divider < UINT8_MAX; clk_divider++) {
    /* Find clock_division to fit current frequency */
    uint32_t clock_div = div_u32u32(clock, clk_divider);
    wrap = div_u32u32(clock_div, freq);
    if (div_u32u32(clock_div, UINT16_MAX) <= freq && wrap <= UINT16_MAX) {
      break;
    }
  }

  if (clk_divider < UINT8_MAX) {
    /* Only considering whole number division */
    pwm_set_clkdiv_int_frac(slice_num, clk_divider, 0);
    pwm_set_enabled(slice_num, true);
    pwm_set_wrap(slice_num, (uint16_t)wrap);
    pwm_set_chan_level(slice_num, chan,
                       (uint16_t)div_u32u32((((uint16_t)(duty_cycle == 100 ? (wrap + 1) : wrap)) * duty_cycle), 100));
  }
  else {
    return -2;
  }

  return 1;
}

static int backlight_freq = 100000;
static int display_v2_backlight_function = 0;

void set_display_v2_backlight_unlock(int on) {
  display_v2_backlight_function = on;
}

int get_display_v2_backlight_unlocked() {
  return display_v2_backlight_function;
}

void set_display_backlight_freq(int freq) {
  if (freq < 0) freq = 0;
  backlight_freq = freq;
}

// this functionality is only for the second type of display for Pocket Reform
// that will ship in late 2024 (TOP070F01A)
// called from timer interrupt, no sleep allowed here!
void set_display_backlight(int percent) {
  if (!display_v2_backlight_function) {
    return;
  }

  // DISP_EN = 7 = PWM3 B
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_PWM);
  pwm_set_freq_duty(pwm_gpio_to_slice_num(PIN_DISP_EN), pwm_gpio_to_channel(PIN_DISP_EN), backlight_freq, percent);

  if (mb_version() < 2) {
    // caveat: latch needs to be always-on
    // for brightnesses other than full brightness to work
    if (percent == 0 || percent == 100) {
      gpio_put(PIN_PWREN_LATCH, 1);
      gpio_put(PIN_PWREN_LATCH, 0);
    } else {
      gpio_put(PIN_PWREN_LATCH, 1);
    }
  }
}

void charger_tick();

// Assumes mps_reg_status and battery_info->charge_percentage were recently updated.
//
static void derive_emergency_charge_necessary(void) {
  if (mps_reg_status.status.chg_stat == MPS_CHGSTAT_TRICKLE && battery_info.charge_percentage == 0) {
    battery_info.emergency_charge_necessary = true;
  } else {
    battery_info.emergency_charge_necessary = false;
  }
}

void charger_init() {
  // TODO: check all MP2650 registers, esp. 4, 7, b
  mps_fault_last = 0;

  mps_read_buf(MPS_REGSTART_STATUS, sizeof(mps_reg_status.all_regs), mps_reg_status.all_regs);
  derive_emergency_charge_necessary();

  // reset all registers
  mps_reg_config.config0.reg_rst = 1;
  mps_write_byte(MPS_REG_CONFIG0, mps_reg_config.config0.reg_byte);
  busy_wait_us(50);

  // It should be possible to read this after writing to MPS_REG_CONFIG0 below, but apparently then we read the resetted registers (again?).
  mps_read_buf(MPS_REGSTART_CONFIG, sizeof(mps_reg_config.all_regs), mps_reg_config.all_regs);

  if (battery_info.emergency_charge_necessary) {
    // continue emergency charging
    mps_reg_config.config0.chg_en = 1;
  } else {
    // turn off charging until PD allows it
    mps_reg_config.config0.chg_en = 0;
  }
  mps_reg_config.config0.susp_en = 0;
  mps_reg_config.config0.ntc_gcomp_sel = 0;  // disable OTG pin
  mps_write_byte(MPS_REG_CONFIG0, mps_reg_config.config0.reg_byte);

  // 160sec watchdog
  mps_reg_config.config1.wtd = 3;
  mps_write_byte(MPS_REG_CONFIG1, mps_reg_config.config1.reg_byte);

  mps_read_buf(MPS_REGSTART_LIMITS, sizeof(mps_reg_limits.all_regs), mps_reg_limits.all_regs);
  mps_read_buf(MPS_REGSTART_STATUS, sizeof(mps_reg_status.all_regs), mps_reg_status.all_regs);

  // 2A max charge current, assumes 4000mAh cells.
  mps_reg_limits.charge_current = 1<<5 | 1<<3;
  // set all current limits to 500mA (should always be safe)
  int current_limit = 1<<3 | 1<<1;
  mps_reg_limits.input_i_limit1 = current_limit;
  mps_write_buf(MPS_REGSTART_LIMITS, sizeof(mps_reg_limits.all_regs), mps_reg_limits.all_regs);
  mps_read_buf(MPS_REGSTART_LIMITS, sizeof(mps_reg_limits.all_regs), mps_reg_limits.all_regs);
  mps_write_byte(0x0F, current_limit); // Input Limit 2

  gpio_put(PIN_LED_R, mps_reg_config.config0.chg_en);

  charger_tick();
}

void charger_tick() {
  derive_emergency_charge_necessary();

  uint8_t old_config3 = mps_reg_config.config3.reg_byte;
  if (battery_info.som_is_powered) {
    mps_reg_config.config3.prochot_psys_cfg = 0b11;  // enable PSYS/ADC feature, even on battery
  } else {
    mps_reg_config.config3.prochot_psys_cfg = 0;  // save power
  }

  if (old_config3 != mps_reg_config.config3.reg_byte) {
    mps_write_byte(MPS_REG_CONFIG3, mps_reg_config.config3.reg_byte);
  }

  // TODO: do this only every X seconds, maybe up to 100?
  // clear watchdog
  mps_reg_config.config0.wtd_rst = 1;
  mps_write_byte(MPS_REG_CONFIG0, mps_reg_config.config0.reg_byte);
}

// current in 10mA units
void charger_enable_charge(int current) {
  int current_reg_value = current / 5;
  printf("# [charger] setting limit %d \n", current_reg_value);

  mps_reg_limits.input_i_limit1 = current_reg_value;
  mps_write_buf(MPS_REGSTART_LIMITS, sizeof(mps_reg_limits.all_regs), mps_reg_limits.all_regs);
  mps_read_buf(MPS_REGSTART_LIMITS, sizeof(mps_reg_limits.all_regs), mps_reg_limits.all_regs);
  mps_write_byte(0x0F, current_reg_value);

  mps_reg_config.config0.chg_en = 1;
  mps_reg_config.config0.susp_en = 0;
  mps_write_byte(MPS_REG_CONFIG0, mps_reg_config.config0.reg_byte);
  mps_fault_last = 0;

  gpio_put(PIN_LED_R, 1);
}

void charger_disable_charge() {
  mps_reg_config.config0.chg_en = 0;
  // "USB Suspended Mode" means the DC/DC routing VIN to system power isn't used
  // TODO: on mb2.0, this could probably be turned on?
  mps_reg_config.config0.susp_en = 0;
  mps_write_byte(MPS_REG_CONFIG0, mps_reg_config.config0.reg_byte);

  // set all current limits to 500mA (should always be safe)
  int current_limit = 1<<3 | 1<<1;
  mps_reg_limits.input_i_limit1 = current_limit;
  mps_write_buf(MPS_REGSTART_LIMITS, sizeof(mps_reg_limits.all_regs), mps_reg_limits.all_regs);
  mps_read_buf(MPS_REGSTART_LIMITS, sizeof(mps_reg_limits.all_regs), mps_reg_limits.all_regs);
  mps_write_byte(0x0F, current_limit); // Input Limit 2

  gpio_put(PIN_LED_R, 0);
}

void gauge_tick(battery_info_s *batinfo) {
  // read devname to identify if communication works
  uint16_t max17320_devname = max_read_word(0x21);
  if (max17320_devname == 0x4209 || max17320_devname == 0x420a || max17320_devname == 0x420b) {
    batinfo->max17320_devname = max17320_devname;
  } else {
    //printf("# [battery] [ERROR] gauge did not respond\n");
    batinfo->max17320_devname = 0;
    batinfo->input_volts = -1;
    batinfo->time_to_empty = 0;
    batinfo->charge_percentage = 0;
    batinfo->cell1_volts = 0;
    batinfo->cell2_volts = 0;
    batinfo->time_to_empty = 0;
    batinfo->battery_amps = 0;
    batinfo->battery_volts = 0;
    return;
  }

  // disable write protection (CommStat)
  max_write_word(0x61, 0x0000);
  max_write_word(0x61, 0x0000);
  // set pack cfg: 2 cells (0), 1+1 thermistor, 6v charge pump, 11:thtype=10k, btpken on, no aoldo
  max_write_word_100(0xb5, (0 << 14) | (1 << 13) | (0 << 11) | (0 << 8) | (2 << 2) | 0);

  // enable balancing (zener)
  // nBalCfg
  max_write_word(0x61, 0x0000);
  max_write_word(0x61, 0x0000);
  max_write_word_100(0xd4, (1 << 13) | (3 << 10) | (3 << 5));

  uint16_t comm_stat = max_read_word(0x61);
  uint16_t status = max_read_word(0x00);

  uint16_t packcfg = max_read_word_100(0xb5);

  uint16_t prot_status = max_read_word(0xd9);
  uint16_t prot_alert = max_read_word(0xaf);
  uint16_t prot_cfg2 = max_read_word_100(0xf1);
  uint16_t therm_cfg = max_read_word_100(0xca);
  float avg_vcell = max_word_to_mv(max_read_word(0x19));
  float vcell = max_word_to_mv(max_read_word(0x1a));
  float current = max_word_to_ma_signed(max_read_word(0x1c));
  float avg_current = max_word_to_ma_signed(max_read_word(0x1d));
  float cell1 = max_word_to_mv(max_read_word(0xd8));
  float cell2 = max_word_to_mv(max_read_word(0xd7));
  float cell3 = max_word_to_mv(max_read_word(0xd6));
  float cell4 = max_word_to_mv(max_read_word(0xd5));
  // this value looks good (checked with inducing voltages w/ power supply)
  float vpack = max_word_to_pack_mv(max_read_word(0xda));

  float temp = ((float)((int16_t)max_read_word(0x1b))) * (1.0 / 256.0);
  float die_temp = ((float)((int16_t)max_read_word(0x34))) * (1.0 / 256.0);
  float temp1 = ((float)((int16_t)max_read_word_100(0x3a))) * (1.0 / 256.0);
  float temp2 = ((float)((int16_t)max_read_word_100(0x39))) * (1.0 / 256.0);
  float temp3 = ((float)((int16_t)max_read_word_100(0x38))) * (1.0 / 256.0);
  float temp4 = ((float)((int16_t)max_read_word_100(0x37))) * (1.0 / 256.0);

  float rep_capacity = max_word_to_cap(max_read_word(0x05));
  float rep_percentage = max_word_to_percentage(max_read_word(0x06));
  float rep_age = max_word_to_percentage(max_read_word(0x07));
  float rep_full_capacity = max_word_to_cap(max_read_word(0x10));
  float rep_time_to_empty = max_word_to_time(max_read_word(0x11));
  float rep_time_to_full = max_word_to_time(max_read_word(0x20));

  batinfo->battery_amps = -current/1000.0;
  batinfo->battery_volts = vpack/1000.0;
  batinfo->gauge_avg_milliamps = avg_current;

  batinfo->charge_percentage = (int)rep_percentage;
  // charger mostly doesn't charge to >98%
  if (batinfo->charge_percentage >= 98) {
    batinfo->charge_percentage = 100;
  }
  batinfo->cell1_volts = cell1;
  batinfo->cell2_volts = cell2;
  batinfo->time_to_empty = rep_time_to_empty;

  if (batinfo->print_pack_info) {
    printf("[pack_info]\n");
    printf("comm_stat = 0x%04x\n", comm_stat);
    printf("packcfg = 0x%04x\n", packcfg);
    printf("status = 0x%04x\n", status);
    printf("status_prot_alert = %d\n", (status & 0x8000) ? 1 : 0);
    printf("prot_alert = 0x%04x\n", prot_alert);
    printf("prot_cfg2 = 0x%04x\n", prot_cfg2);
    printf("therm_cfg = 0x%04x\n", therm_cfg);
    printf("temp = %f\n", temp);
    printf("die temp = %f\n", die_temp);
    printf("temp1 = %f ", temp1);
    printf("temp2 = %f ", temp2);
    printf("temp3 = %f ", temp3);
    printf("temp4 = %f\n", temp4);

    printf("prot_status = 0x%04x\n", prot_status);

    printf("prot_status_meaning = \"");
    if (prot_status & (1 << 14)) {
      printf("too hot, ");
    }
    if (prot_status & (1 << 13)) {
      printf("full, ");
    }
    if (prot_status & (1 << 12)) {
      printf("too cold for charge, ");
    }
    if (prot_status & (1 << 11)) {
      printf("overvoltage, ");
    }
    if (prot_status & (1 << 10)) {
      printf("overcharge current, ");
    }
    if (prot_status & (1 << 9)) {
      printf("qoverflow, ");
    }
    if (prot_status & (1 << 8)) {
      printf("prequal timeout, ");
    }
    if (prot_status & (1 << 7)) {
      printf("imbalance, ");
    }
    if (prot_status & (1 << 6)) {
      printf("perm fail, ");
    }
    if (prot_status & (1 << 5)) {
      printf("die hot, ");
    }
    if (prot_status & (1 << 4)) {
      printf("too hot for discharge, ");
    }
    if (prot_status & (1 << 3)) {
      printf("undervoltage, ");
    }
    if (prot_status & (1 << 2)) {
      printf("overdischarge current, ");
    }
    if (prot_status & (1 << 1)) {
      printf("resdfault, ");
    }
    if (prot_status & (1 << 0)) {
      printf("ship, ");
    }
    printf("\"\n");

    printf("vcell = %f\n", vcell);
    printf("avg_vcell = %f\n", avg_vcell);
    printf("cell1 = %f ", cell1);
    printf("cell2 = %f ", cell2);
    printf("cell3 = %f ", cell3);
    printf("cell4 = %f ", cell4);
    printf("vpack = %f\n", vpack);

    printf("rep_capacity_mah = %f\n", rep_capacity);
    printf("rep_percentage = %f\n", rep_percentage);
    printf("rep_age_percentage = %f\n", rep_age);
    printf("rep_full_capacity_mah = %f\n", rep_full_capacity);
    printf("rep_time_to_empty_sec = %f\n", rep_time_to_empty);
    printf("rep_time_to_full_sec = %f\n", rep_time_to_full);
  }

  if (status & 0x0002) {
    printf("# POR, clearing status\n");
    max_write_word(0x61, 0x0000);
    max_write_word(0x61, 0x0000);
    max_write_word(0x00, status & (~0x0002));
  }
}

void gauge_init() {
  gauge_tick(&battery_info);
}

void charger_dump(battery_info_s *batinfo) {
  // TODO: if max reports overvoltage (disbalanced cells),
  // can we lower the charging voltage temporarily?
  // alternatively, the current

  // Read charging status every 10ms
  if (batinfo->ticks % 100 != 0) {
    return;
  }

  mps_read_buf(MPS_REGSTART_STATUS, sizeof(mps_reg_status.all_regs), mps_reg_status.all_regs);
  if (mps_reg_status.fault.reg_byte != 0) {
    mps_fault_last = mps_reg_status.fault.reg_byte;
  }

  // Read ADC values and update stuff every 100ms
  if (batinfo->ticks % 1000 != 0) {
    return;
  }

  mps_read_buf(MPS_REGSTART_ADC, sizeof(mps_reg_adc.all_regs), mps_reg_adc.all_regs);
  float adc_input_v = mps_word_to_12800(mps_reg_adc.input_v);

  // carry over to globals for SPI reporting
  batinfo->input_volts = adc_input_v;

  if (batinfo->print_pack_info) {
    float adc_sys_v = mps_word_to_6400(mps_reg_adc.sys_v);
    float adc_input_i = mps_word_to_3200(mps_reg_adc.input_i);
    float adc_discharge_c = mps_word_to_6400(mps_reg_adc.bat_discharge_i);

    float adc_bat_v = mps_word_to_6400(mps_reg_adc.bat_v);
    float adc_charge_c = mps_word_to_6400(mps_reg_adc.bat_charge_i);
    float adc_temp = mps_word_to_temp(mps_reg_adc.junction_t);
    float adc_sys_pwr = mps_word_to_watt(mps_reg_adc.sys_p);
    float adc_ntc_v = mps_word_to_ntc(mps_read_word(0x40));

    printf("[charger_info]\n");
    printf("status = 0x%x ", mps_reg_status.status.reg_byte);
    printf("fault = 0x%x\n", mps_reg_status.fault.reg_byte);

    printf("adc_bat_v = %f ", adc_bat_v);
    printf("adc_sys_v = %f ", adc_sys_v);
    printf("bat_full_v = 0x%d\n", mps_reg_limits.reg04.v_batt_reg);

    printf("adc_charge_c = %f\n", adc_charge_c);
    printf("adc_input_v = %f ", adc_input_v);
    printf("adc_input_i = %f\n", adc_input_i);
    printf("adc_temp = %f\n", adc_temp);
    printf("adc_sys_pwr = %f ", adc_sys_pwr);
    printf("adc_discharge_c = %f\n", adc_discharge_c);
    printf("adc_ntc_v = %f\n", adc_ntc_v);

    printf("input_i_limit1 = 0x%x ", mps_reg_limits.input_i_limit1);
    printf("input_v_limit = 0x%x\n", mps_reg_limits.input_v_limit);

    printf("charge_c = 0x%x ", mps_reg_limits.charge_current);
    printf("precharge_c = 0x%x\n", mps_reg_limits.reg03.i_pre);
  }
}

void charger_led_indication(battery_info_s *batinfo) {
  // update LED every 10ms, although data might be older
  if (batinfo->ticks % 100 != 0) {
    return;
  }

  static float phase = 0.0f;

  if (mps_reg_status.status.chg_stat == MPS_CHGSTAT_FAST) {
    uint pwm_max = 0xffff;
    if (gpio_get_function(PIN_LED_R) != GPIO_FUNC_PWM) {
      gpio_set_function(PIN_LED_R, GPIO_FUNC_PWM);
      pwm_set_wrap(PIN_LED_R_PWM_SLICE, pwm_max);
      pwm_set_enabled(PIN_LED_R_PWM_SLICE, true);
    }

    float sine_val = sin(phase);
    float normalized = (sine_val + 1.0f) / 2.0f;
    float breathing_curve = normalized * normalized;
    // 10%..100% brightness (i.e. never completely off)
    breathing_curve = 0.1f + (breathing_curve * 0.9f);
    uint16_t pwm_level = (uint16_t)(breathing_curve * pwm_max);
    pwm_set_chan_level(PIN_LED_R_PWM_SLICE, PIN_LED_R_PWM_CHAN, pwm_level);
    phase += 0.02f;
    if (phase > 2.0f * M_PI) {
      phase = 0.0f;
    }
  } else {
    phase = 0.0f;
    if (gpio_get_function(PIN_LED_R) != GPIO_FUNC_SIO) {
      gpio_set_function(PIN_LED_R, GPIO_FUNC_SIO);
    }
    if (mps_reg_status.status.chg_stat == MPS_CHGSTAT_OFF || mps_reg_status.status.chg_stat == MPS_CHGSTAT_DONE) {
      // not charging
      gpio_put(PIN_LED_R, 0);
    } else {
      // trickle charging
      gpio_put(PIN_LED_R, 1);
    }
  }
}

void som_power_indication() {
  pwm_set_freq_duty(PIN_LED_B_PWM_SLICE, PIN_LED_B_PWM_CHAN, 25000, battery_info.som_is_powered ? 30 : 0);
}

void set_usb_mode(int port, int mode) {
  if (port == 1) {
    switch (mode) {
    case 0:
      // usb2: sysctl external (default when off)
      usb_host_5v_set(1, 0);
      gpio_ext_uswitch_enable(0);
      gpio_ext_uswitch_enable(1);
      gpio_ext_uswitch_enable(2);
      break;
    case 1:
      // usb2: normal usb host, sysctl internal (default when on)
      gpio_ext_uswitch_disable(0);
      gpio_ext_uswitch_disable(1);
      gpio_ext_uswitch_disable(2);
      // enable 5v power on the port
      usb_host_5v_set(1, 1);
      break;
    case 2:
      // usb2: EDL external
      usb_host_5v_set(1, 0);
      gpio_ext_uswitch_enable(0);
      gpio_ext_uswitch_disable(1);
      gpio_ext_uswitch_enable(2);
      break;
    }
  }
  else if (port == 0) {
    switch (mode) {
    case 0:
      // usb1: SoC UART
      // TODO: should be controlled only by PD logic
      gpio_ext_uswitch_enable(3);
      break;
    case 1:
      // usb1: host
      // TODO: should be controlled only by PD logic
      gpio_ext_uswitch_disable(3);
      break;
    }
  }
}

void turn_som_power_on_v20() {
  printf("# [action] turn_som_power_on_v20\n");

  set_boot_magic();

  gpio_set_dir(PIN_USB_LOADER_SW, GPIO_OUT);
  gpio_put(PIN_USB_LOADER_SW, 1);

  gpio_ext_enable(GPIO_EXT_5V_EN);
  gpio_ext_enable(GPIO_EXT_3V3_EN);
  gpio_ext_enable(GPIO_EXT_HUB_PWR_EN);
  gpio_ext_enable(GPIO_EXT_PCIE_PWR_EN);
  gpio_ext_disable(GPIO_EXT_USWITCH_OFF);

  // Modem
  gpio_put(PIN_FLIGHTMODE, 1);  // active low
  gpio_put(PIN_MODEM_RESET, 1); // active low
  gpio_put(PIN_MODEM_POWER, 1); // active high
  gpio_put(PIN_PHONE_DPR, 1);   // active high

  // Display reset (deassert)
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_SIO);
  gpio_put(PIN_DISP_EN, 1);
  gpio_ext_enable(GPIO_EXT_DISP_RESET_N);
  gpio_ext_enable(GPIO_EXT_DISP_BL_PWR_EN);
  gpio_ext_enable(GPIO_EXT_DISP_1EN_2BL);

  if (!battery_info.som_is_powered) {
    // reset display + modem (active low)
    // unless this is a warm reboot of sysctl
    gpio_ext_disable(GPIO_EXT_DISP_RESET_N);
    gpio_put(PIN_MODEM_RESET, 0);
    busy_wait_us(20*1000);
    gpio_ext_enable(GPIO_EXT_DISP_RESET_N);
    gpio_put(PIN_MODEM_RESET, 1);
  }

  battery_info.som_is_powered = true;
  som_power_indication();
  init_spi_client();

  // Connect SC internally to SoC
  // USB port 2 becomes USB host
  set_usb_mode(1, 1);

  // has an effect only on dispv2
  set_display_backlight(30);

  // in case the soc is sleeping, wake it
  // FIXME don't do this after warm boot!
  // som_wake();
}

void turn_som_power_off_v20() {
  printf("# [action] turn_som_power_off\n");
  battery_info.som_is_powered = false;

  clear_boot_magic();

  // Modem
  gpio_put(PIN_FLIGHTMODE, 0);  // active low
  gpio_put(PIN_MODEM_RESET, 0); // active low
  gpio_put(PIN_MODEM_POWER, 0); // active high
  gpio_put(PIN_PHONE_DPR, 0);   // active high

  // Display reset (assert)
  gpio_ext_disable(GPIO_EXT_DISP_RESET_N);
  gpio_ext_disable(GPIO_EXT_DISP_BL_PWR_EN);
  gpio_ext_disable(GPIO_EXT_DISP_1EN_2BL);

  // Power rails
  gpio_ext_disable(GPIO_EXT_5V_EN);
  gpio_ext_disable(GPIO_EXT_3V3_EN);
  gpio_ext_disable(GPIO_EXT_HUB_PWR_EN);
  gpio_ext_disable(GPIO_EXT_PCIE_PWR_EN);
  gpio_ext_disable(GPIO_EXT_USWITCH_OFF);

  // TODO
  gpio_put(PIN_USB_LOADER_SW, 0);
  gpio_set_dir(PIN_USB_LOADER_SW, GPIO_IN);

  // has an effect only on dispv2
  set_display_backlight(0);
  set_display_v2_backlight_unlock(0);
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_SIO);
  gpio_put(PIN_DISP_EN, 0);

  som_power_indication();

  // Expose SC as device on USB port 2
  set_usb_mode(1, 0);
}

void turn_som_power_on() {
  printf("# [action] turn_som_power_on\n");

  if (mb_version() >= 2) {
    turn_som_power_on_v20();
    return;
  }

  gpio_put(PIN_PWREN_LATCH, 0);

  set_boot_magic();

  gpio_put(PIN_1V1_ENABLE, 1);
  gpio_put(PIN_3V3_ENABLE, 1);
  gpio_put(PIN_5V_ENABLE, 1);

  // Modem
  gpio_put(PIN_FLIGHTMODE, 1);  // active low
  gpio_put(PIN_MODEM_RESET, 1); // active low
  gpio_put(PIN_MODEM_POWER, 1); // active high
  gpio_put(PIN_PHONE_DPR, 1);   // active high

  // Display reset (deassert)
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_SIO);
  gpio_put(PIN_DISP_RESET, 1);
  gpio_put(PIN_DISP_EN, 1);

  // Latch power enables
  gpio_put(PIN_PWREN_LATCH, 1);
  if (!battery_info.som_is_powered) {
    // reset display + modem (active low)
    // unless this is a warm reboot of sysctl
    gpio_put(PIN_DISP_RESET, 0);
    gpio_put(PIN_MODEM_RESET, 0);
    busy_wait_us(20*1000);
    gpio_put(PIN_DISP_RESET, 1);
    gpio_put(PIN_MODEM_RESET, 1);
  }
  gpio_put(PIN_PWREN_LATCH, 0);

  battery_info.som_is_powered = true;
  som_power_indication();
  init_spi_client();

  // has an effect only on dispv2
  set_display_backlight(30);
}

/*
  this function can be called from a timer interrupt
  in the spi command handler, no sleep is allowed here.
  if delays should become necessary, they have to be
  busy loops.
 */
void turn_som_power_off() {
  printf("# [action] turn_som_power_off\n");

  if (mb_version() >= 2) {
    turn_som_power_off_v20();
    return;
  }

  battery_info.som_is_powered = false;
  clear_boot_magic();

  // Display
  gpio_put(PIN_DISP_RESET, 0);
  // has an effect only on dispv2
  set_display_backlight(0);
  set_display_v2_backlight_unlock(0);
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_SIO);
  gpio_put(PIN_DISP_EN, 0);

  // Modem
  gpio_put(PIN_FLIGHTMODE, 0);  // active low
  gpio_put(PIN_MODEM_RESET, 0); // active low
  gpio_put(PIN_MODEM_POWER, 0); // active high
  gpio_put(PIN_PHONE_DPR, 0);   // active high

  // Power rails
  gpio_put(PIN_5V_ENABLE, 0);
  gpio_put(PIN_3V3_ENABLE, 0);
  gpio_put(PIN_1V1_ENABLE, 0);

  // Latch power enables
  gpio_put(PIN_PWREN_LATCH, 1);
  gpio_put(PIN_PWREN_LATCH, 0);

  som_power_indication();
}

void som_wake() {
  // reset the display before wakeup
  gpio_ext_disable(GPIO_EXT_DISP_RESET_N);
  busy_wait_us(20*1000);
  gpio_ext_enable(GPIO_EXT_DISP_RESET_N);

  gpio_set_dir(PIN_SOM_WAKE, GPIO_OUT);
  gpio_put(PIN_SOM_WAKE, 1);
  busy_wait_us(5*1000);
  gpio_put(PIN_SOM_WAKE, 0);
  busy_wait_us(5*1000);
  gpio_put(PIN_SOM_WAKE, 1);
  gpio_set_dir(PIN_SOM_WAKE, GPIO_IN);
}

// reset register
#define AIRCR (*((volatile uint32_t*)(PPB_BASE + 0x0ED0C)))

// mostly adapted from pico-extras/src/rp2_common/pico_sleep/sleep.c
void enter_powersave(void) {
  printf("# [enter_powersave]\n");
  // wake on gpio interrupt, mode level (vs edge) and active low
  uint32_t event = IO_BANK0_DORMANT_WAKE_INTE0_GPIO0_LEVEL_LOW_BITS;

  uart_deinit(UART_ID);
  gpio_init(PIN_KBD_UART_RX);
  gpio_init(PIN_KBD_UART_TX);
  gpio_set_input_enabled(PIN_KBD_UART_RX, true);
  gpio_set_input_enabled(PIN_KBD_UART_TX, true);
  gpio_set_dormant_irq_enabled(PIN_KBD_UART_RX, event, true);

  gpio_init(PIN_FUSB_INT);
  gpio_set_input_enabled(PIN_FUSB_INT, true);
  gpio_set_pulls(PIN_FUSB_INT, true, false); // pullup
  gpio_set_dormant_irq_enabled(PIN_FUSB_INT, event, true);

  uint src_hz = 6500 * KHZ;
  clock_configure(clk_ref, CLOCKS_CLK_REF_CTRL_SRC_VALUE_ROSC_CLKSRC_PH, 0, src_hz, src_hz);

  // CLK_SYS = CLK_REF
  clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF, 0, src_hz, src_hz);

  clock_configure(clk_rtc,
                  0, // No GLMUX
                  CLOCKS_CLK_RTC_CTRL_AUXSRC_VALUE_ROSC_CLKSRC_PH,
                  src_hz,
                  46875);

  // CLK PERI = clk_sys. Used as reference clock for Peripherals. No dividers so just select and enable
  clock_configure(clk_peri,
                  0,
                  CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                  src_hz,
                  src_hz);

  // Disable USB and ADC clocks.
  clock_stop(clk_usb);
  clock_stop(clk_adc);

  // Disable PLLs.
  pll_deinit(pll_sys);
  pll_deinit(pll_usb);

  xosc_disable();

  clocks_hw->sleep_en0 = CLOCKS_SLEEP_EN0_CLK_RTC_RTC_BITS;
  clocks_hw->sleep_en1 = 0x0;

  scb_hw->scr |= ARM_CPU_PREFIXED(SCR_SLEEPDEEP_BITS);
  rosc_set_dormant();

  // reset
  AIRCR = 0x5fa0004;
  while (true) {
    __wfi();
  }
}

static int global_mb_version = 1;
int mb_version() {
  return global_mb_version;
}

void setup_gpios() {
  // UART to keyboard
  uart_init(UART_ID, BAUD_RATE);
  uart_set_format(UART_ID, DATA_BITS, STOP_BITS, PARITY);
  uart_set_hw_flow(UART_ID, false, false);
  uart_set_fifo_enabled(UART_ID, true);
  gpio_set_function(PIN_KBD_UART_TX, GPIO_FUNC_UART);
  gpio_set_function(PIN_KBD_UART_RX, GPIO_FUNC_UART);

  // UART to som
  uart_init(uart0, BAUD_RATE);
  uart_set_format(uart0, DATA_BITS, STOP_BITS, PARITY);
  uart_set_hw_flow(uart0, false, false);
  uart_set_fifo_enabled(uart0, true);
  gpio_set_function(PIN_SOM_UART_TX, GPIO_FUNC_UART);
  gpio_set_function(PIN_SOM_UART_RX, GPIO_FUNC_UART);

  // i2c to charger and max chips
  gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
  gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
  bi_decl(bi_2pins_with_func(PIN_SDA, PIN_SCL, GPIO_FUNC_I2C));
  i2c_init(i2c0, 100 * 1000);

  // RGB LED
  gpio_init(PIN_LED_R);  // Red is used as charge indicator
  gpio_init(PIN_LED_G);
  gpio_init(PIN_LED_B);  // Blue is used as SOM-powered indicator
  gpio_set_dir(PIN_LED_R, GPIO_OUT);
  gpio_set_dir(PIN_LED_G, GPIO_OUT);
  gpio_set_dir(PIN_LED_B, GPIO_OUT);
  gpio_set_function(PIN_LED_B, GPIO_FUNC_PWM);

  if (gpio_ext_setup(!syscon_warm_boot())) {
    printf("# [setup_gpios] mainboard 2.0+ detected.\n");
    global_mb_version = 2;
  } else {
    printf("# [setup_gpios] mainboard 1.0 detected.\n");
    global_mb_version = 1;
  }

  // Power regulator pins
  if (mb_version() < 2) {
    // on MB2, these functions have been moved
    // to external GPIO controller
    gpio_init(PIN_1V1_ENABLE);
    gpio_init(PIN_3V3_ENABLE);
    gpio_init(PIN_5V_ENABLE);
    gpio_set_dir(PIN_1V1_ENABLE, GPIO_OUT);
    gpio_set_dir(PIN_3V3_ENABLE, GPIO_OUT);
    gpio_set_dir(PIN_5V_ENABLE, GPIO_OUT);
    gpio_put(PIN_1V1_ENABLE, 0);
    gpio_put(PIN_3V3_ENABLE, 0);
    gpio_put(PIN_5V_ENABLE, 0);

    // Power enable latch
    gpio_init(PIN_PWREN_LATCH);
    gpio_set_dir(PIN_PWREN_LATCH, 1);
    gpio_put(PIN_PWREN_LATCH, 0);
  }

  // SoM / SoC wake GPIO
  gpio_init(PIN_SOM_WAKE);
  gpio_set_dir(PIN_SOM_WAKE, GPIO_IN);
  gpio_put(PIN_SOM_WAKE, 0);

  // Display control pins
  gpio_init(PIN_DISP_RESET);
  gpio_set_dir(PIN_DISP_RESET, GPIO_OUT);
  gpio_put(PIN_DISP_RESET, 0);
  gpio_init(PIN_DISP_EN);
  gpio_set_dir(PIN_DISP_EN, GPIO_OUT);
  // For brightness control of display v2,
  // this pin is switched to PWM mode later
  // Needs to be at 100% for display v1
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_SIO);

  // Modem control pins
  gpio_init(PIN_FLIGHTMODE);
  gpio_init(PIN_MODEM_POWER);
  gpio_init(PIN_MODEM_RESET);
  gpio_init(PIN_PHONE_DPR);
  gpio_set_dir(PIN_FLIGHTMODE, GPIO_OUT);
  gpio_set_dir(PIN_MODEM_POWER, GPIO_OUT);
  gpio_set_dir(PIN_MODEM_RESET, GPIO_OUT);
  gpio_set_dir(PIN_PHONE_DPR, GPIO_OUT);
  gpio_put(PIN_FLIGHTMODE, 0);  // active low
  gpio_put(PIN_MODEM_POWER, 0); // active high
  gpio_put(PIN_MODEM_RESET, 0); // active low (?)
  gpio_put(PIN_PHONE_DPR, 0);   // active high // causes 0.146W power use when high in off state!

  // Turn off RGB LED (B is PWMed?)
  gpio_put(PIN_LED_R, 0);
  gpio_put(PIN_LED_G, 0);

  if (mb_version() >= 2) {
    gpio_init(PIN_USB_SRC1_ENABLE);
    gpio_set_dir(PIN_USB_SRC1_ENABLE, GPIO_OUT);
    gpio_put(PIN_USB_SRC1_ENABLE, 0);
    gpio_init(PIN_USB_SRC2_ENABLE);
    gpio_set_dir(PIN_USB_SRC2_ENABLE, GPIO_OUT);
    gpio_put(PIN_USB_SRC2_ENABLE, 0);
  } else if (mb_version() < 2) {
    // USB-C port 1 power rail
    gpio_init(PIN_USB_SRC_ENABLE);
    gpio_set_dir(PIN_USB_SRC_ENABLE, GPIO_OUT);
    gpio_put(PIN_USB_SRC_ENABLE, 0);
  }

  // USB-C port 1 (charger port) CC controller
  gpio_init(PIN_FUSB_INT);
  gpio_set_pulls(PIN_FUSB_INT, true, false); // pullup
  gpio_set_dir(PIN_FUSB_INT, GPIO_IN);

  if (mb_version() >= 2) {
    // EDL/USB loader switch for SoC
    // TODO: should be an output
    gpio_init(PIN_USB_LOADER_SW);
    gpio_set_dir(PIN_USB_LOADER_SW, GPIO_IN);
    // USB-C DisplayPort HPD
    gpio_init(PIN_V20_DP_HPD);
    gpio_set_dir(PIN_V20_DP_HPD, GPIO_OUT);
    gpio_put(PIN_V20_DP_HPD, 0);
  }
}

void setup() {
  // reset if main loop is stuck for 10000ms
  watchdog_enable(10000, 1);
  init_spi_client();

  tusb_init();
  reform_stdio_usb_init();

  printf("# [reset] cause: %#.8x\n", (uint16_t)vreg_and_chip_reset_hw->chip_reset);
  printf("# [reset] magic: %#.8x%.8x\n", (uint16_t)watchdog_hw->scratch[2], (uint16_t)watchdog_hw->scratch[3]);

  if (mb_version() >= 2) {
    /*if (!gpio_ext_setup(!syscon_warm_boot())) {
      printf("# [setup] error: gpio_ext_setup() failed. PCA9557 not responding?\n");
    }*/
    if (!gpio_ext_uswitch_setup(!syscon_warm_boot())) {
      printf("# [setup] error: gpio_ext_uswitch_setup() failed. PCA9536 not responding?\n");
    }
  }

  set_display_backlight_freq(100000);
  // default to display v1, until driver unlocks this
  set_display_v2_backlight_unlock(0);

  // if this is a warm boot, then we need to avoid latching the PWR and display
  // pins.
  if (syscon_warm_boot()) {
    // on by default after reboot
    printf("# [reset] watchdog scratch had valid on magic, restoring power.\n");
    battery_info.som_is_powered = true;
    turn_som_power_on();
  } else {
    // off by default
    turn_som_power_off();
  }
  gauge_init();
  charger_init();
  pd_init();
}

// TODO move line editor to library file
// TODO tab completion
#define CLI_USB_STATE_NORMAL 0
#define CLI_USB_STATE_ESC 1
#define CLI_USB_STATE_CSI 2
#define CLI_USB_LINE_MAX 128
static int cli_usb_state = CLI_USB_STATE_NORMAL;
static char cli_usb_line_hist[CLI_USB_LINE_MAX];
static char cli_usb_line_temp[CLI_USB_LINE_MAX];
static char cli_usb_line[CLI_USB_LINE_MAX];
static int cli_usb_line_cursor = 0;
static struct cli_context cli_ctx_usb;

void cli_usb_line_del() {
  int rewind = 0;
  for (int i = cli_usb_line_cursor; i < CLI_USB_LINE_MAX-1; i++) {
    char c = cli_usb_line[i];
    char next = cli_usb_line[i + 1];
    if (c > 13 && next > 13) {
      cli_usb_line[i] = next;
      putchar(next);
      rewind++;
    } else {
      cli_usb_line[i] = 0;
      putchar(' ');
      rewind++;
      break;
    }
  }
  for (int i = 0; i < rewind; i++) {
    putchar(8);
  }
}

void cli_usb_line_reset() {
  for (int i = 0; i < cli_usb_line_cursor; i++) {
    putchar(8);
    putchar(' ');
    putchar(8);
  }
  cli_usb_line_cursor = 0;
}

void cli_usb_line_restore() {
  for (int i = 0; i < CLI_USB_LINE_MAX; i++) {
    if (cli_usb_line[i] < 13) break;
    putchar(cli_usb_line[i]);
    cli_usb_line_cursor++;
  }
}

void handle_usb_commands() {
  int usb_c = getchar_timeout_us(0);
  if (usb_c != PICO_ERROR_TIMEOUT) {

    if (cli_usb_state == CLI_USB_STATE_ESC) {
      //printf("esc: [0x%x]\n", usb_c);
      if (usb_c == '[') {
        cli_usb_state = CLI_USB_STATE_CSI;
        return;
      }
      if (usb_c >= 0x40 && usb_c <= 0x7e) {
        // "final byte"
        cli_usb_state = CLI_USB_STATE_NORMAL;
        return;
      }
      return;
    }
    if (cli_usb_state == CLI_USB_STATE_CSI) {
      //printf("csi: [0x%x]\n", usb_c);
      if (usb_c >= 0x40 && usb_c <= 0x7e) {
        // "final byte"
        if (usb_c == 0x44) {
          // cursor left
          if (cli_usb_line_cursor > 0) {
            putchar(8);
            cli_usb_line_cursor--;
          }
        } else if (usb_c == 0x43) {
          // cursor right
          if (cli_usb_line_cursor < CLI_USB_LINE_MAX-1) {
            if (cli_usb_line[cli_usb_line_cursor] > 13) {
              putchar(cli_usb_line[cli_usb_line_cursor]);
              cli_usb_line_cursor++;
            }
          }
        } else if (usb_c == 0x41 || usb_c == 0x42) {
          // cursor up or down
          cli_usb_line_reset();
          cli_usb_line_restore();
          cli_usb_line_reset();
          memcpy(cli_usb_line_temp, cli_usb_line, CLI_USB_LINE_MAX);
          memcpy(cli_usb_line, cli_usb_line_hist, CLI_USB_LINE_MAX);
          memcpy(cli_usb_line_hist, cli_usb_line_temp, CLI_USB_LINE_MAX);
          cli_usb_line_restore();
        } else if (usb_c == 0x7e) {
          // del
          cli_usb_line_del();
        }
        cli_usb_state = CLI_USB_STATE_NORMAL;
        return;
      }
      return;
    }
    if (usb_c == 0x1b) {
      // TODO: handle escape sequence
      cli_usb_state = CLI_USB_STATE_ESC;
      return;
    }

    if (usb_c == 13) usb_c = 10;
    if (usb_c == 0x7f || usb_c == 8) {
      // backspace
      if (cli_usb_line_cursor > 0) {
        putchar(8);
        putchar(' ');
        putchar(8);
        cli_usb_line_cursor--;
        cli_usb_line_del();
        //cli_usb_line[cli_usb_line_cursor] = 0;
      }
      return;
    }

    if (cli_usb_line_cursor < CLI_USB_LINE_MAX-1) {
      if (usb_c != 10) {
        char c = cli_usb_line[cli_usb_line_cursor];

        if (c > 13) {
          // in the middle of a line, insert and shift
          if (cli_usb_line[CLI_USB_LINE_MAX-2] == 0) {
            for (int i = CLI_USB_LINE_MAX - 2; i >= cli_usb_line_cursor; i--) {
              cli_usb_line[i + 1] = cli_usb_line[i];
            }
            int rewind = 0;
            for (int i = cli_usb_line_cursor; i < CLI_USB_LINE_MAX; i++) {
              char d = cli_usb_line[i];
              if (d > 13) {
                putchar(d);
                rewind++;
              }
            }
            for (int i = 0; i < rewind; i++) {
              putchar(8);
            }
            cli_usb_line[cli_usb_line_cursor] = usb_c;
            cli_usb_line_cursor++;
          }
        } else {
          // end of line
          cli_usb_line[cli_usb_line_cursor] = usb_c;
          cli_usb_line_cursor++;
        }
      }
      putchar(usb_c);
    }

    if (usb_c == 10) {
      for (int i=0; i<CLI_USB_LINE_MAX; i++) {
        // feed line into CLI
        if (cli_usb_line[i] == 0) {
          cli_usb_line[i] = 10;
        }

        cli_char(&cli_ctx_usb, cli_usb_line[i]);
        int resp_len = cli_get_out_pos(&cli_ctx_usb);
        if (resp_len > 0) {
          const char *cli_out_buf = cli_get_out(&cli_ctx_usb);
          printf("%s\n", cli_out_buf);
          cli_reset_out(&cli_ctx_usb);
        }

        if (cli_usb_line[i] == 0 || cli_usb_line[i] == 10) {
          // done
          cli_usb_line_cursor = 0;
          memcpy(cli_usb_line_hist, cli_usb_line, i);
          cli_usb_line_hist[i] = 0;
          memset(cli_usb_line, 0, CLI_USB_LINE_MAX);
          memset(cli_usb_line_temp, 0, CLI_USB_LINE_MAX);
          return;
        }
      }
    }
  }
}

// TODO: abstract MPS communication away into a charger file
void usb_host_5v_set(int port, int enable) {
  enable = !!enable;
  if (mb_version() < 2) {
#ifndef OTG_AS_5V
    gpio_put(PIN_USB_SRC_ENABLE, enable);
#else
    // NOTE: obsolete, can only be used on charger v1.0
    mps_reg_config.config0.otg_en = enable;
    mps_write_byte(MPS_REG_CONFIG0, mps_reg_config.config0.reg_byte);
#endif
  } else if (mb_version() >= 2) {
    //printf("# [usb_host_5v_enable]\n");
    if (port == 1) {
      gpio_put(PIN_USB_SRC2_ENABLE, enable);
    } else {
      gpio_put(PIN_USB_SRC1_ENABLE, enable);
    }
  }
}

// TODO: replace with actual timer
#define TICK_MS 1

void loop() {
  bool can_sleep = true;

  watchdog_update();

  // handle commands from keyboard UART
  handle_uart_commands();

#ifdef ACM_ENABLED
  // handle commands over USB serial
  handle_usb_commands();
#endif

  // for SPI CS signal debugging
#if 0
  int cs_state = gpio_get(PIN_SOM_SS0);
  if (cs_state != cs_state_prev) {
    printf("# cs = %d\n", cs_state);
  }
  cs_state_prev = cs_state;
#endif

  if (!pd_tick(&battery_info)) {
    can_sleep = false;
  }
  // TODO: way too much traffic here
  charger_tick();
  battery_info.ticks++;

  // every 100ms: query gauge, update battery status
  if (battery_info.ticks % (100*TICK_MS) == 0) {
    gauge_tick(&battery_info);
  }
  charger_dump(&battery_info);
  charger_led_indication(&battery_info);

  // every 1000ms: report to serial
  if (battery_info.print_pack_info && (battery_info.ticks % (1000*TICK_MS) == 0)) {
    // TODO: print adc_charge_c adc_discharge_c
    printf("# %s %s %s chg=%1x mps_flt=%02x/%02x input=%0.2fmV@%0.2fmA charge=%0.2fmA discharge=%0.2fmA p=%0.2fW ttempty=%umin battery=%0.4fV@%0.4fmA battery_avg=%0.4fmA battery_pwr=%0.4fmW\n",
           battery_info.som_is_powered ? "ON" : "OFF",
           mps_reg_status.status.acok ? "AC" : "BAT",
           mps_reg_config.config0.chg_en ? "CHG" : "",
           mps_reg_status.status.chg_stat,
           mps_reg_status.fault.reg_byte,
           mps_fault_last,
           mps_word_to_12800(mps_reg_adc.input_v),
           mps_word_to_3200(mps_reg_adc.input_i),
           mps_word_to_6400(mps_reg_adc.bat_charge_i),
           mps_word_to_6400(mps_reg_adc.bat_discharge_i),
           mps_word_to_watt(mps_reg_adc.sys_p),
           (unsigned int)battery_info.time_to_empty/60,
           battery_info.battery_volts,
           battery_info.battery_amps*1000,
           battery_info.gauge_avg_milliamps,
           battery_info.gauge_avg_milliamps*battery_info.battery_volts
           );
  }

  if (can_sleep) {
    // NOTE: sleep functions disrupt USB UART
    // sleep 1ms
    busy_wait_us(1000);

    // TODO: make configurable
    if ((battery_info.ticks % (5000 * TICK_MS) == 0)
        && !battery_info.som_is_powered
        && battery_info.gauge_avg_milliamps < 0) {
      // If 5 seconds have passed while the device is off and
      // on battery, conserve power and halt.
      // It will exit powersave when detecting a USB-C PD plug event
      // or keyboard interaction.
      // FIXME powersave turned off for debugging
      //enter_powersave();
    }
  }
}

void mntre_reset_callback(void) {
  // avoid leaving display brightness PWM at a bad duty cycle.
  // TODO: only relevant for MB 1.0?
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_SIO);
  gpio_put(PIN_DISP_EN, 1);

  if (mb_version() < 2) {
    // clear latch, so resetting _us_ does not reset the SOC.
    gpio_put(PIN_PWREN_LATCH, 0);
  }
}

void sysctl_disable_irqs() {
  irq_set_enabled(IO_IRQ_BANK0, false);
}

void sysctl_enable_irqs() {
  irq_set_enabled(IO_IRQ_BANK0, true);
}

// from pico-sdk docs:
// https://www.raspberrypi.com/documentation/pico-sdk/hardware.html#function-documentation-10
// IRQ handlers set up with gpio_set_irq... are acknowledged automatically.
void spi_commands_task([[maybe_unused]] unsigned int gpio, [[maybe_unused]] long unsigned int event) {
  // handle commands from SoM
  // TODO: pass cli state/handle
  if (gpio != PIN_SOM_SS0) return;
  sysctl_disable_irqs();
  handle_spi_commands(&battery_info);
  sysctl_enable_irqs();
}

int main() {
  // TODO: saves a bit of power, but causes ROSC dormant trouble
  //set_sys_clock_48mhz();
  setup_gpios();
  setup();
  cli_init_env();
  hwapi_pocket_init(&battery_info);
  uart_com_init();
#ifdef ACM_ENABLED
  cli_init(&cli_ctx_usb);
#endif

  // by default, present sysctl usb to SoC USB internally
  hwapi_set_usb_mode(0, 1);
  hwapi_set_usb_mode(1, 1);

  gpio_set_irq_enabled_with_callback(PIN_SOM_SS0, GPIO_IRQ_EDGE_FALL|GPIO_IRQ_EDGE_RISE, true, &spi_commands_task);

  printf("# [pocket_sysctl] entering main loop\n");

  while (true) {
    loop();
  }

  return 0;
}
