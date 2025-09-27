/**
 * SPI commands from the SOM
 *
 * Ported from MNT Reform reform2-lpc-fw.
 */

#include "hardware/resets.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "pico/timeout_helper.h"
#include "spi_com.h"

// this is spi_write_blocking from pico-sdk, with timeout handling.
static int our_spi_write_timeout_us(spi_inst_t *spi, const uint8_t *src, size_t len, uint timeout_us) {
  absolute_time_t until = make_timeout_time_us(timeout_us);
  bool timeout = false;

  invalid_params_if(HARDWARE_SPI, 0 > (int)len);
  // Write to TX FIFO whilst ignoring RX, then clean up afterward. When RX
  // is full, PL022 inhibits RX pushes, and sets a sticky flag on
  // push-on-full, but continues shifting. Safe if SSPIMSC_RORIM is not set.
  for (size_t i = 0; i < len; ++i) {
      while (!timeout && !spi_is_writable(spi)) {
        timeout = time_reached(until);
        tight_loop_contents();
      }
      if (!timeout) {
        spi_get_hw(spi)->dr = (uint32_t)src[i];
      }
  }
  // Drain RX FIFO, then wait for shifting to finish (which may be *after*
  // TX FIFO drains), then drain RX FIFO again
  while (!timeout && spi_is_readable(spi)) {
    timeout = time_reached(until);
    (void)spi_get_hw(spi)->dr;
  }
  while (!timeout && spi_get_hw(spi)->sr & SPI_SSPSR_BSY_BITS) {
    timeout = time_reached(until);
    tight_loop_contents();
  }
  while (!timeout && spi_is_readable(spi)) {
    timeout = time_reached(until);
    (void)spi_get_hw(spi)->dr;
  }

  // Don't leave overrun flag set
  spi_get_hw(spi)->icr = SPI_SSPICR_RORIC_BITS;

  if (timeout) {
    return PICO_ERROR_TIMEOUT;
  }
  return (int)len;
}


void init_spi_client()
{
  gpio_set_function(PIN_SOM_MOSI, GPIO_FUNC_SPI);
  gpio_set_function(PIN_SOM_MISO, GPIO_FUNC_SPI);
  gpio_set_function(PIN_SOM_SS0, GPIO_FUNC_SPI);
  gpio_set_function(PIN_SOM_SCK, GPIO_FUNC_SPI);

  // 4 MHz
  spi_init(spi1, 4000 * 1000);
  // we don't appreciate the wording, but it's the API we are given
  spi_set_slave(spi1, true);
  spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);

  printf("# [spi] init_spi_client done\n");
}

#define SPI_DEBUG_ENABLED 0

void handle_spi_commands(battery_info_s *battery_info)
{
  uint8_t spi_command = 0;
  uint8_t spi_arg1 = 0;
  uint8_t spi_buf[SPI_BUF_LEN]; // normally 8 bytes
  int spi_rxlen = 0;

  if (!battery_info->som_is_powered) return;
  if (!spi_is_readable(spi1)) return;

  // non blocking read
  for (uint8_t i = 0; i < 4; i++) {
    // read a byte (but don't write a byte)
    uint8_t rx = (uint8_t)spi_get_hw(spi1)->dr;
    spi_buf[i] = rx;
    spi_rxlen++;
  }

  // commands are always 4 bytes, starting with 0xb5
  // dump the buffer to serial
  if (SPI_DEBUG_ENABLED || spi_buf[0] != 0xb5 || spi_rxlen != 4) {
    printf("# [spi rx %d] ", spi_rxlen);
    for (int i = 0; i < spi_rxlen; i++) {
      printf("%2x ", spi_buf[i]);
    }
    printf("\t");
    for (int i = 0; i < spi_rxlen; i++) {
      if (spi_buf[i] >= 32) {
        printf("%c", spi_buf[i]);
      } else {
        printf(".");
      }
    }
    printf("\n");
    // reset SPI0 block
    // this is a workaround for confusion with
    // software spi from BPI-CM4 where we get
    // bit-shifted bytes
    printf("# [spi resync]\n");
    init_spi_client();
    return;
  }

  spi_command = spi_buf[1];
  spi_arg1 = spi_buf[2];

  // clear receive buffer, reuse as send buffer
  memset(spi_buf, 0, SPI_BUF_LEN);

  if (spi_command == 'f') {
    // return firmware version and api info
    if (spi_arg1 == 0) memcpy(spi_buf, FW_STRING1, MIN(SPI_BUF_LEN, sizeof(FW_STRING1)));
    else if (spi_arg1 == 1) memcpy(spi_buf, FW_STRING2, MIN(SPI_BUF_LEN, sizeof(FW_STRING2)));
    else memcpy(spi_buf, MNTRE_FIRMWARE_VERSION, MIN(SPI_BUF_LEN, sizeof(MNTRE_FIRMWARE_VERSION)));
  }
  else if (spi_command == 'q') {
    // execute status query command
    uint8_t percentage = (uint8_t)battery_info->charge_percentage;
    int16_t voltsInt = (int16_t)(battery_info->battery_volts * 1000.0);
    int16_t currentInt = (int16_t)(battery_info->battery_amps * 1000.0);

    spi_buf[0] = (uint8_t)voltsInt;
    spi_buf[1] = (uint8_t)(voltsInt >> 8);
    spi_buf[2] = (uint8_t)currentInt;
    spi_buf[3] = (uint8_t)(currentInt >> 8);
    spi_buf[4] = (uint8_t)percentage;
    // TODO "state" not implemented
    spi_buf[5] = (uint8_t)0;
  }
  else if (spi_command == 'v') {
    // get cell voltage
    if (spi_arg1 == 0) {
      // pack 0
      int volts = battery_info->cell1_volts;
      spi_buf[0] = (uint8_t)volts;
      spi_buf[1] = (uint8_t)(volts >> 8);

      volts = battery_info->cell2_volts;
      spi_buf[2] = (uint8_t)volts;
      spi_buf[3] = (uint8_t)(volts >> 8);
    }
  }
  else if (spi_command == 'c') {
    // get calculated capacity (emulated)
    uint16_t cap_accu = (uint16_t)BATTERY_CAPACITY_MILLIAMP_HOURS * (((float)battery_info->charge_percentage) / 100.0);
    uint16_t cap_min = (uint16_t)0;
    uint16_t cap_max = (uint16_t)BATTERY_CAPACITY_MILLIAMP_HOURS;

    spi_buf[0] = (uint8_t)cap_accu;
    spi_buf[1] = (uint8_t)(cap_accu >> 8);
    spi_buf[2] = (uint8_t)cap_min;
    spi_buf[3] = (uint8_t)(cap_min >> 8);
    spi_buf[4] = (uint8_t)cap_max;
    spi_buf[5] = (uint8_t)(cap_max >> 8);
  }
  else if (spi_command == 'p') {
    // toggle system power off
    if (spi_arg1 == 1) {
      turn_som_power_off();
      // don't try to send a response to turned-off SOM,
      // because SPI will hang otherwise
      return;
    }
  }
  else if (spi_command == 'z') {
    // pass message byte (spi_arg1) directly to uart (implemented for Desktop Reform control panel)
    /* TODO: not yet implemented */
  }
  else if (spi_command == 'b') {
    // only for display v2
    unsigned int brightness = spi_arg1;
    // 80% is a limit of the hardware (above, the backlight can flicker)
    if (brightness > 80)
      brightness = 80;
    set_display_backlight(brightness);
  }

  /* send response to host (8 bytes) and discard response */
  if (battery_info->som_is_powered) {
    if (our_spi_write_timeout_us(spi1, (const uint8_t*)spi_buf, 8, I2C_TIMEOUT) == PICO_ERROR_TIMEOUT) {
      init_spi_client();
    }
  }
}
