/*
  SPDX-License-Identifier: GPL-3.0-or-later
  MNT Pocket Reform System Controller Firmware for RP2040
  Copyright 2023-2024 MNT Research GmbH

  fusb_read/write functions based on:
  https://git.clarahobbs.com/pd-buddy/pd-buddy-firmware/src/branch/master/lib/src/fusb302b.c
*/
#include "sysctl.h"
#include "pico/divider.h"

pd_state_s pd_state = {0};
battery_info_s battery_info = {0};
int disp_bl_percent = 50;

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
  uint32_t clock_div = 0;
  uint32_t clock = clock_get_hz(clk_sys);

  if (freq < 8 || freq > clock)
    /* This is the frequency range of generating a PWM
       in RP2040 at 125MHz */
    return -1;

  for (clk_divider = 1; clk_divider < UINT8_MAX; clk_divider++)
  {
    /* Find clock_division to fit current frequency */
    clock_div = div_u32u32(clock, clk_divider);
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
void set_display_backlight(int percent)
{
#ifdef PREF_DISPLAY_V2
  // DISP_EN = 7 = PWM3 B
  printf("# set_display_backlight: %d", percent);
  pwm_set_freq_duty(pwm_gpio_to_slice_num(PIN_DISP_EN), pwm_gpio_to_channel(PIN_DISP_EN), 100000, percent);
#else
  (void)percent; // supress unused message
#endif
}

void charger_configure()
{
  // TODO: check all MP2650 registers, esp. 4, 7, b

  // set input current limit to 2000mA
  mps_write_byte(0x00, (1 << 5) | (1 << 3));
  // set input voltage limit to 6V (above 5V USB voltage)
  mps_write_byte(0x01, (1 << 6));
  // set charge current limit to 2000mA (1600+400)
  mps_write_byte(0x02, (1 << 5) | (1 << 3));
}

void gauge_dump(battery_info_s *battery_info)
{
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
    printf("temp1 = %f\n", temp1);
    printf("temp2 = %f\n", temp2);
    printf("temp3 = %f\n", temp3);
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
    printf("cell1 = %f\n", cell1);
    printf("cell2 = %f\n", cell2);
    printf("cell3 = %f\n", cell3);
    printf("cell4 = %f\n", cell4);
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

int gauge_identify(battery_info_s *battery_info)
{
  // read devname to identify if communication works
  uint16_t max17320_devname = max_read_word(0x21);
  if (max17320_devname == 0x4209 || max17320_devname == 0x420a || max17320_devname == 0x420b)
  {
    battery_info->max17320_devname = max17320_devname;
    return 1;
  }
  return 0;
}

void charger_dump(battery_info_s *battery_info)
{
  // TODO: if max reports overvoltage (disbalanced cells),
  // can we lower the charging voltage temporarily?
  // alternatively, the current

  uint8_t status = mps_read_byte(0x13);
  uint8_t fault = mps_read_byte(0x14);

  float adc_bat_v = mps_word_to_6400(mps_read_word(0x16)) / 1000.0;
  float adc_sys_v = mps_word_to_6400(mps_read_word(0x18)) / 1000.0;
  float adc_charge_c = mps_word_to_6400(mps_read_word(0x1a)) / 1000.0;
  float adc_input_v = mps_word_to_12800(mps_read_word(0x1c)) / 1000.0;
  float adc_input_c = mps_word_to_3200(mps_read_word(0x1e)) / 1000.0;
  float adc_temp = mps_word_to_temp(mps_read_word(0x24));
  float adc_sys_pwr = mps_word_to_w(mps_read_word(0x26));
  float adc_discharge_c = mps_word_to_6400(mps_read_word(0x28)) / 1000.0;
  float adc_ntc_v = mps_word_to_ntc(mps_read_word(0x40)) / 1000.0;

  uint8_t input_c_limit = mps_read_byte(0x00);
  uint8_t input_v_limit = mps_read_byte(0x01);
  uint8_t charge_c = mps_read_byte(0x02);
  uint8_t precharge_c = mps_read_byte(0x03);
  uint8_t bat_full_v = mps_read_byte(0x04);

  // carry over to globals for SPI reporting
  battery_info->battery_amps = -(adc_input_c - adc_discharge_c);
  battery_info->battery_volts = adc_sys_v;
  battery_info->input_volts = adc_input_v;

  if (battery_info->print_pack_info)
  {
    printf("[charger_info]\n");
    printf("status = 0x%x\n", status);
    printf("fault = 0x%x\n", fault);

    printf("adc_bat_v = %f\n", adc_bat_v);
    printf("adc_sys_v = %f\n", adc_sys_v);
    printf("adc_charge_c = %f\n", adc_charge_c);
    printf("adc_input_v = %f\n", adc_input_v);
    printf("adc_input_c = %f\n", adc_input_c);
    printf("adc_temp = %f\n", adc_temp);
    printf("adc_sys_pwr = %f\n", adc_sys_pwr);
    printf("adc_discharge_c = %f\n", adc_discharge_c);
    printf("adc_ntc_v = %f\n", adc_ntc_v);

    printf("input_c_limit = 0x%x\n", input_c_limit);
    printf("input_v_limit = 0x%x\n", input_v_limit);
    printf("charge_c = 0x%x\n", charge_c);
    printf("precharge_c = 0x%x\n", precharge_c);
    printf("bat_full_v = 0x%d\n", bat_full_v);
  }
}

void turn_som_power_on()
{
  init_spi_client();

  // Power latch enable
  gpio_put(PIN_PWREN_LATCH, 1);

  gpio_put(PIN_LED_B, 1);

  set_boot_magic();

  printf("# [action] turn_som_power_on\n");
  gpio_put(PIN_1V1_ENABLE, 1);
  sleep_ms(10);
  gpio_put(PIN_3V3_ENABLE, 1);
  sleep_ms(10);
  gpio_put(PIN_5V_ENABLE, 1);

  // Modem
  gpio_put(PIN_FLIGHTMODE, 1);  // active low
  gpio_put(PIN_MODEM_RESET, 0); // active low (?)
  gpio_put(PIN_MODEM_POWER, 1); // active high
  gpio_put(PIN_PHONE_DPR, 1);   // active high

#ifdef PREF_DISPLAY_V2
  set_display_backlight(50);
#else
  gpio_put(PIN_DISP_EN, 1);
#endif

  sleep_ms(10);
  gpio_put(PIN_DISP_RESET, 1);

  // Modem
  gpio_put(PIN_MODEM_RESET, 1); // active low

#ifdef PREF_DISPLAY_V2
  // FIXME: can't stop the latch when using non-100% brightness
#else
  // Power latch end
  gpio_put(PIN_PWREN_LATCH, 0);
#endif

  battery_info.som_is_powered = true;
}

void turn_som_power_off()
{
  init_spi_client();

  // Power latch enable
  gpio_put(PIN_PWREN_LATCH, 1);

  gpio_put(PIN_LED_B, 0);

  clear_boot_magic();

  printf("# [action] turn_som_power_off\n");
  gpio_put(PIN_DISP_RESET, 0);

#ifdef PREF_DISPLAY_V2
  set_display_backlight(0);
#else
  gpio_put(PIN_DISP_EN, 0);
#endif

  // Modem
  gpio_put(PIN_FLIGHTMODE, 0);  // active low
  gpio_put(PIN_MODEM_RESET, 0); // active low
  gpio_put(PIN_MODEM_POWER, 0); // active high
  gpio_put(PIN_PHONE_DPR, 0);   // active high

  // Power rails
  gpio_put(PIN_5V_ENABLE, 0);
  sleep_ms(10);
  gpio_put(PIN_3V3_ENABLE, 0);
  sleep_ms(10);
  gpio_put(PIN_1V1_ENABLE, 0);

  // Power latch end
  gpio_put(PIN_PWREN_LATCH, 0);

  battery_info.som_is_powered = false;
}

void som_wake()
{
  uart_puts(uart0, "wake\r\n");
}

void setup()
{
  stdio_init_all();
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
  gpio_init(PIN_LED_R);
  gpio_init(PIN_LED_G);
  gpio_init(PIN_LED_B);
  gpio_set_dir(PIN_LED_R, GPIO_OUT);
  gpio_set_dir(PIN_LED_G, GPIO_OUT);
  gpio_set_dir(PIN_LED_B, GPIO_OUT);

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

#ifdef PREF_DISPLAY_V2
  gpio_set_function(PIN_DISP_EN, GPIO_FUNC_PWM);
#else
  gpio_put(PIN_DISP_EN, 0);
#endif

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
  gpio_put(PIN_LED_B, 0);

  // USB charger-port power rail
  gpio_init(PIN_USB_SRC_ENABLE);
  gpio_set_dir(PIN_USB_SRC_ENABLE, GPIO_OUT);
  gpio_put(PIN_USB_SRC_ENABLE, 0);

  // if this is a warm boot, then we need to avoid latching the PWR and display
  // pins.
  if (syscon_warm_boot())
  {
    printf("# [reset] watchdog scratch had valid on magic, not latching power.\n");
    battery_info.som_is_powered = true;
  }
  else
  {
    gpio_put(PIN_PWREN_LATCH, 1);
    gpio_put(PIN_PWREN_LATCH, 0);
  }

  // gpio_init(PIN_FUSB_INT);
  // gpio_set_dir(PIN_FUSB_INT, GPIO_IN);
  // gpio_pull_up(PIN_FUSB_INT);

  // gpio_set_irq_callback(gpio_callback);
  // gpio_set_irq_enabled(PIN_FUSB_INT, GPIO_IRQ_EDGE_FALL, true);
  // irq_set_enabled(IO_IRQ_BANK0, true);

  charger_configure();

  // clear interrupt registers
  // fusb_read_byte(FUSB_INTERRUPT);
  // fusb_read_byte(FUSB_INTERRUPTA);
  // fusb_read_byte(FUSB_INTERRUPTB);
  // sleep_ms(10);

  // // reset
  // fusb_write_byte(FUSB_RESET, FUSB_RESET_SW_RES);
  // sleep_us(10);

  // // enable dac for measuring CC lines
  // fusb_write_byte(FUSB_POWER, 0x0F);

  // // global interrupt mask enabled by default, remove it
  // // keep default for host current to port (500 ma)
  // // fusb_write_byte(FUSB_CONTROL0, (0x01 << FUSB_CONTROL0_HOST_CUR_SHIFT));

  // // default pull down resistors
  // fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_PDWN_2 | FUSB_SWITCHES0_PDWN_1);

  pd_state.next_state = PD_STATE_RESET_IDLE;
}

void handle_usb_commands()
{
  int usb_c = getchar_timeout_us(0);
  if (usb_c != PICO_ERROR_TIMEOUT && isprint(usb_c))
  {
    printf("# [acm_command] '%c'\n", usb_c);
    if (usb_c == '1')
    {
      turn_som_power_on();
    }
    else if (usb_c == '0')
    {
      turn_som_power_off();
    }
    else if (usb_c == 'p')
    {
      battery_info.print_pack_info = !battery_info.print_pack_info;
    }
    else if (usb_c == 'r')
    {
      pd_state.next_state = PD_STATE_RESET_IDLE;
    }
    else if (usb_c == 'R')
    {
      pd_state.next_state = PD_STATE_RESET_IDLE;
      fusb_write_byte(FUSB_CONTROL3,
                      FUSB_CONTROL3_SEND_HARD_RESET |
                          FUSB_CONTROL3_AUTO_HARDRESET |
                          FUSB_CONTROL3_AUTO_SOFTRESET |
                          (3 << FUSB_CONTROL3_N_RETRIES_SHIFT) |
                          FUSB_CONTROL3_AUTO_RETRY);
      sleep_ms(1);
      fusb_write_byte(FUSB_CONTROL3,
                      FUSB_CONTROL3_AUTO_HARDRESET |
                          FUSB_CONTROL3_AUTO_SOFTRESET |
                          (3 << FUSB_CONTROL3_N_RETRIES_SHIFT) |
                          FUSB_CONTROL3_AUTO_RETRY);
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

#ifdef FACTORY_MODE
int factory_turn_on_once = 1;
#endif

// void on_uart_rx() {
//   handle_uart_commands(&battery_info);
// }

void loop()
{
  // handle commands from keyboard
  handle_uart_commands(&battery_info);

  // handle commands from SoM
  handle_spi_commands(&battery_info);

#ifdef ACM_ENABLED
  // handle commands over usb serial
  handle_usb_commands();
#endif

  handle_pd_state(&battery_info, &pd_state);

  battery_info.ticks++;
  if (battery_info.ticks > 200)
  {
    battery_info.ticks = 0;
    if (gauge_identify(&battery_info))
    {
      charger_dump(&battery_info);
      gauge_dump(&battery_info);
    }
    else
    {
      printf("# [battery] [ERROR] gauge did not respond\n");
      battery_info.input_volts = -1;
      battery_info.max17320_devname = 0;
    }
  }

  sleep_ms(10); // one tick is effectively 10 ms
  pd_state.ticks++;
}

int main()
{
  setup();

  sleep_ms(1000);
  printf("# [pocket_sysctl] entering main loop\n");

  while (true)
  {
    loop();
  }

  return 0;
}
