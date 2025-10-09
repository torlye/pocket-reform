/*
  SPDX-License-Identifier: GPL-3.0-or-later
  MNT Pocket Reform System Controller Firmware for RP2040
  Copyright 2023-2024 MNT Research GmbH

  fusb_read/write functions based on:
  https://git.clarahobbs.com/pd-buddy/pd-buddy-firmware/src/branch/master/lib/src/fusb302b.c
*/
#include <math.h>
#include "sysctl.h"
#include "pico/divider.h"
#include "tusb.h"
#include "reform_stdio_usb.h"

battery_info_s battery_info = {0};
int disp_bl_percent = 100;

// The Pico boot rom uses watchdog scratch registers 0, 1, 4, 5, 6, and 7.
// That leaves 2 and 3 for our "system is on" magic.
// A _real_ power-on reset clears these registers, so if our magic is left over
// then we have either been updated while the system is on, or have run into an
// event with probability 2**-64.
bool syscon_warm_boot()
{
  return (watchdog_hw->scratch[2] == BOOT_MAGIC_2 &&
          watchdog_hw->scratch[3] == BOOT_MAGIC_3);
}

void set_boot_magic()
{
  watchdog_hw->scratch[2] = BOOT_MAGIC_2;
  watchdog_hw->scratch[3] = BOOT_MAGIC_3;
}

void clear_boot_magic()
{
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
int32_t pwm_set_freq_duty(uint32_t slice_num, uint32_t chan, uint32_t freq, int duty_cycle)
{
  uint8_t clk_divider = 0;
  uint32_t wrap = 0;
  uint32_t clock = clock_get_hz(clk_sys);

  if (freq < 8 || freq > clock)
    /* This is the frequency range of generating a PWM
       in RP2040 at 125MHz */
    return -1;

  for (clk_divider = 1; clk_divider < UINT8_MAX; clk_divider++)
  {
    /* Find clock_division to fit current frequency */
    uint32_t clock_div = div_u32u32(clock, clk_divider);
    wrap = div_u32u32(clock_div, freq);
    if (div_u32u32(clock_div, UINT16_MAX) <= freq && wrap <= UINT16_MAX)
    {
      break;
    }
  }

  if (clk_divider < UINT8_MAX)
  {
    /* Only considering whole number division */
    pwm_set_clkdiv_int_frac(slice_num, clk_divider, 0);
    pwm_set_enabled(slice_num, true);
    pwm_set_wrap(slice_num, (uint16_t)wrap);
    pwm_set_chan_level(slice_num, chan,
                       (uint16_t)div_u32u32((((uint16_t)(duty_cycle == 100 ? (wrap + 1) : wrap)) * duty_cycle), 100));
  }
  else
    return -2;

  return 1;
}

// this functionality is only for the second type of display for Pocket Reform
// that will ship in late 2024 (TOP070F01A)
// called from timer interrupt, no sleep allowed here!
void set_display_backlight(int percent)
{
  // DISP_EN = 7 = PWM3 B
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_PWM);
  pwm_set_freq_duty(pwm_gpio_to_slice_num(PIN_DISP_EN), pwm_gpio_to_channel(PIN_DISP_EN), 100000, percent);

  // caveat: latch needs to be always-on
  // for brightnesses other than full brightness to work
  if (percent == 0 || percent == 100) {
    gpio_put(PIN_PWREN_LATCH, 0);
  } else {
    gpio_put(PIN_PWREN_LATCH, 1);
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

void charger_init()
{
  // TODO: check all MP2650 registers, esp. 4, 7, b

  mps_read_buf(MPS_REGSTART_STATUS, sizeof(mps_reg_status.all_regs), mps_reg_status.all_regs);
  derive_emergency_charge_necessary();

  // reset all registers
  mps_reg_config.config0.reg_rst = 1;
  mps_write_byte(MPS_REG_CONFIG0, mps_reg_config.config0.reg_byte);
  sleep_us(50);

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

  gpio_put(PIN_LED_R, 1);
}

void charger_disable_charge() {
  mps_reg_config.config0.chg_en = 0;
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

void gauge_tick(battery_info_s *battery_info)
{
  // read devname to identify if communication works
  uint16_t max17320_devname = max_read_word(0x21);
  if (max17320_devname == 0x4209 || max17320_devname == 0x420a || max17320_devname == 0x420b)
  {
    battery_info->max17320_devname = max17320_devname;
  } else {
    printf("# [battery] [ERROR] gauge did not respond\n");
    battery_info->max17320_devname = 0;
    battery_info->input_volts = -1;
    battery_info->time_to_empty = 0;
    battery_info->charge_percentage = 0;
    battery_info->cell1_volts = 0;
    battery_info->cell2_volts = 0;
    battery_info->time_to_empty = 0;
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
  float vcell = max_word_to_mv(max_read_word(0x1a));
  float avg_vcell = max_word_to_mv(max_read_word(0x19));
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

  battery_info->charge_percentage = (int)rep_percentage;
  // charger mostly doesn't charge to >98%
  if (battery_info->charge_percentage >= 98)
  {
    battery_info->charge_percentage = 100;
  }
  battery_info->cell1_volts = cell1;
  battery_info->cell2_volts = cell2;
  battery_info->time_to_empty = rep_time_to_empty;

  if (battery_info->print_pack_info)
  {
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
    if (prot_status & (1 << 14))
    {
      printf("too hot, ");
    }
    if (prot_status & (1 << 13))
    {
      printf("full, ");
    }
    if (prot_status & (1 << 12))
    {
      printf("too cold for charge, ");
    }
    if (prot_status & (1 << 11))
    {
      printf("overvoltage, ");
    }
    if (prot_status & (1 << 10))
    {
      printf("overcharge current, ");
    }
    if (prot_status & (1 << 9))
    {
      printf("qoverflow, ");
    }
    if (prot_status & (1 << 8))
    {
      printf("prequal timeout, ");
    }
    if (prot_status & (1 << 7))
    {
      printf("imbalance, ");
    }
    if (prot_status & (1 << 6))
    {
      printf("perm fail, ");
    }
    if (prot_status & (1 << 5))
    {
      printf("die hot, ");
    }
    if (prot_status & (1 << 4))
    {
      printf("too hot for discharge, ");
    }
    if (prot_status & (1 << 3))
    {
      printf("undervoltage, ");
    }
    if (prot_status & (1 << 2))
    {
      printf("overdischarge current, ");
    }
    if (prot_status & (1 << 1))
    {
      printf("resdfault, ");
    }
    if (prot_status & (1 << 0))
    {
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

  if (status & 0x0002)
  {
    printf("# POR, clearing status\n");
    max_write_word(0x61, 0x0000);
    max_write_word(0x61, 0x0000);
    max_write_word(0x00, status & (~0x0002));
  }
}

void gauge_init() {
  gauge_tick(&battery_info);
}

void charger_dump(battery_info_s *battery_info)
{
  // TODO: if max reports overvoltage (disbalanced cells),
  // can we lower the charging voltage temporarily?
  // alternatively, the current

  // Read charging status every 10ms
  if (battery_info->ticks % 100 != 0) {
    return;
  }

  mps_read_buf(MPS_REGSTART_STATUS, sizeof(mps_reg_status.all_regs), mps_reg_status.all_regs);

  // Read ADC values and update stuff every 100ms
  if (battery_info->ticks % 1000 != 0) {
    return;
  }

  mps_read_buf(MPS_REGSTART_ADC, sizeof(mps_reg_adc.all_regs), mps_reg_adc.all_regs);

  uint16_t adc_sys_v = mps_word_to_6400(mps_reg_adc.sys_v);
  uint16_t adc_input_i = mps_word_to_3200(mps_reg_adc.input_i);
  uint16_t adc_discharge_c = mps_word_to_6400(mps_reg_adc.bat_discharge_i);
  uint16_t adc_input_v = mps_word_to_12800(mps_reg_adc.input_v);

  // carry over to globals for SPI reporting
  battery_info->battery_amps = -(float)(adc_input_i - adc_discharge_c)/(float)1000.0;
  battery_info->battery_volts = (float)adc_sys_v/(float)1000.0;
  battery_info->input_volts = adc_input_v;

  if (battery_info->print_pack_info) {
    uint16_t adc_bat_v = mps_word_to_6400(mps_reg_adc.bat_v);
    uint16_t adc_charge_c = mps_word_to_6400(mps_reg_adc.bat_charge_i);
    float adc_temp = mps_word_to_temp(mps_reg_adc.junction_t);
    float adc_sys_pwr = mps_word_to_watt(mps_reg_adc.sys_p);
    float adc_ntc_v = mps_word_to_ntc(mps_read_word(0x40));

    printf("[charger_info]\n");
    printf("status = 0x%x ", mps_reg_status.status.reg_byte);
    printf("fault = 0x%x\n", mps_reg_status.fault.reg_byte);

    printf("adc_bat_v = %d ", adc_bat_v);
    printf("adc_sys_v = %d ", adc_sys_v);
    printf("bat_full_v = 0x%d\n", mps_reg_limits.reg04.v_batt_reg);

    printf("adc_charge_c = %d\n", adc_charge_c);
    printf("adc_input_v = %d ", adc_input_v);
    printf("adc_input_c = %d\n", adc_input_i);
    printf("adc_temp = %f\n", adc_temp);
    printf("adc_sys_pwr = %f ", adc_sys_pwr);
    printf("adc_discharge_c = %d\n", adc_discharge_c);
    printf("adc_ntc_v = %f\n", adc_ntc_v);

    printf("input_i_limit1 = 0x%x ", mps_reg_limits.input_i_limit1);
    printf("input_v_limit = 0x%x\n", mps_reg_limits.input_v_limit);

    printf("charge_c = 0x%x ", mps_reg_limits.charge_current);
    printf("precharge_c = 0x%x\n", mps_reg_limits.reg03.i_pre);
  }
}

void charger_led_indication(battery_info_s *battery_info) {
  // update LED every 10ms, although data might be older
  if (battery_info->ticks % 100 != 0) {
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

void turn_som_power_on()
{
  printf("# [action] turn_som_power_on\n");

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
    sleep_ms(20);
    gpio_put(PIN_DISP_RESET, 1);
    gpio_put(PIN_MODEM_RESET, 1);
  }
  gpio_put(PIN_PWREN_LATCH, 0);

  battery_info.som_is_powered = true;
  som_power_indication();
  init_spi_client();
}

/*
  this function can be called from a timer interrupt
  in the spi command handler, no sleep is allowed here.
  if delays should become necessary, they have to be
  busy loops.
*/
void turn_som_power_off()
{
  printf("# [action] turn_som_power_off\n");
  battery_info.som_is_powered = false;

  clear_boot_magic();

  // Display
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_SIO);
  gpio_put(PIN_DISP_EN, 0);
  gpio_put(PIN_DISP_RESET, 0);

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
  set_display_backlight(0);

  som_power_indication();
}

void som_wake()
{
  // TODO: toggle gpio 19!
  uart_puts(uart0, "wake\r\n");
}

void setup()
{
  tusb_init();
  reform_stdio_usb_init();

  // reset if main loop is stuck for 10000ms
  watchdog_enable(10000, 1);

  init_spi_client();

  printf("# [reset] cause: %#.8x\n", (uint16_t)vreg_and_chip_reset_hw->chip_reset);
  printf("# [reset] magic: %#.8x%.8x\n", (uint16_t)watchdog_hw->scratch[2], (uint16_t)watchdog_hw->scratch[3]);

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

  // Power regulator pins
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

  // Display control pins
  gpio_init(PIN_DISP_RESET);
  gpio_init(PIN_DISP_EN);
  gpio_set_dir(PIN_DISP_EN, GPIO_OUT);
  gpio_set_dir(PIN_DISP_RESET, GPIO_OUT);
  gpio_put(PIN_DISP_RESET, 0);

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

  // Turn off RGB LED
  gpio_put(PIN_LED_R, 0);
  gpio_put(PIN_LED_G, 0);

  // USB charger-port power rail
  gpio_init(PIN_USB_SRC_ENABLE);
  gpio_set_dir(PIN_USB_SRC_ENABLE, GPIO_OUT);
  gpio_put(PIN_USB_SRC_ENABLE, 0);

  gpio_init(PIN_FUSB_INT);
  gpio_set_dir(PIN_FUSB_INT, 0);

  // if this is a warm boot, then we need to avoid latching the PWR and display
  // pins.
  if (syscon_warm_boot())
  {
    // on by default after reboot
    printf("# [reset] watchdog scratch had valid on magic, restoring power.\n");
    battery_info.som_is_powered = true;
    turn_som_power_on();
  }
  else
  {
    // off by default
    turn_som_power_off();
  }

  gauge_init();
  charger_init();

  pd_init();
}

void handle_usb_commands()
{
  int usb_c = getchar_timeout_us(0);
  if (usb_c != PICO_ERROR_TIMEOUT && isprint(usb_c))
  {
    printf("# [acm_command] '%c'\n", usb_c);
    if (usb_c == '1')
    {
      if (battery_info.som_is_powered) {
        turn_som_power_on(true);
      } else {
        turn_som_power_on(false);
      }
    }
    else if (usb_c == '0')
    {
      turn_som_power_off();
    }
    else if (usb_c == 'p')
    {
      battery_info.print_pack_info = !battery_info.print_pack_info;
    }
    else if (usb_c == '!')
    {
      // DONOTMERGE
      // test a sysctl hang / watchdog reset
      uint32_t i = 0;
      while (true) {
        printf("# lets try writing to ROM... (0x%08lx)\n", i);
        volatile uint32_t* x = (uint32_t*)i;
        *x = i;
      }
    }
    else if (usb_c == '-')
    {
      // only for PREF_DISPLAY_V2
      disp_bl_percent -= 5;
      if (disp_bl_percent < 0)
        disp_bl_percent = 0;
      set_display_backlight(disp_bl_percent);
    }
    else if (usb_c == '+')
    {
      // only for PREF_DISPLAY_V2
      disp_bl_percent += 5;
      if (disp_bl_percent > 100)
        disp_bl_percent = 100;
      set_display_backlight(disp_bl_percent);
    }
  }
}

void usb_host_5v_enable() {
#ifndef OTG_AS_5V
  gpio_put(PIN_USB_SRC_ENABLE, 1);
#else
  mps_reg_config.config0.otg_en = 1;
  mps_write_byte(MPS_REG_CONFIG0, mps_reg_config.config0.reg_byte);
#endif
}

void usb_host_5v_disable() {
#ifndef OTG_AS_5V
  gpio_put(PIN_USB_SRC_ENABLE, 0);
#else
  mps_reg_config.config0.otg_en = 0;
  mps_write_byte(MPS_REG_CONFIG0, mps_reg_config.config0.reg_byte);
#endif
}

// DONOTMERGE
#define ACM_ENABLED 1

void loop()
{
  bool can_sleep = true;

  watchdog_update();

  // handle commands from keyboard
  handle_uart_commands(&battery_info);

#ifdef ACM_ENABLED
  // handle commands over usb serial
  handle_usb_commands();
#endif

  if (!pd_tick(&battery_info)) {
    can_sleep = false;
  }
  charger_tick();

  battery_info.ticks++;

  // every 100ms: query gauge, update battery status
  if (battery_info.ticks % 1000 == 0)
  {
    gauge_tick(&battery_info);
  }
  charger_dump(&battery_info);
  charger_led_indication(&battery_info);

  // every 1000ms: report to serial
  if (battery_info.ticks % 10000 == 0)
  {
    // TODO: print adc_charge_c adc_discharge_c
    printf("# %s %s %s chg=%1x mps_flt=%02x input=%dmV@%dmA charge=%dmA discharge=%dmA p=%0.2fW ttempty=%umin\n",
            battery_info.som_is_powered ? "ON" : "OFF",
            mps_reg_status.status.acok ? "AC" : "BAT",
            mps_reg_config.config0.chg_en ? "CHG" : "",
            mps_reg_status.status.chg_stat,
            mps_reg_status.fault.reg_byte,
            mps_word_to_12800(mps_reg_adc.input_v),
            mps_word_to_3200(mps_reg_adc.input_i),
            mps_word_to_6400(mps_reg_adc.bat_charge_i),
            mps_word_to_6400(mps_reg_adc.bat_discharge_i),
            mps_word_to_watt(mps_reg_adc.sys_p),
            (unsigned int)battery_info.time_to_empty/60
            );
  }

  if (can_sleep) {
    sleep_us(100); // one tick is 0.1ms
  }
}

void mntre_reset_callback(void) {
  // avoid leaving display brightness PWM at a bad duty cycle.
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_SIO);
  gpio_put(PIN_DISP_EN, 1);

  // clear latch, so resetting _us_ does not reset the SOC.
  gpio_put(PIN_PWREN_LATCH, 0);
}

bool spi_commands_task(__unused struct repeating_timer *t) {
  // handle commands from SoM
  handle_spi_commands(&battery_info);
  // timer should continue calling us
  return true;
}

int main()
{
  setup();

  // call SPI task every 5ms to ensure response time
  struct repeating_timer spi_timer;
  add_repeating_timer_ms(-5, spi_commands_task, NULL, &spi_timer);

  printf("# [pocket_sysctl] entering main loop\n");

  while (true)
  {
    loop();
  }

  return 0;
}
