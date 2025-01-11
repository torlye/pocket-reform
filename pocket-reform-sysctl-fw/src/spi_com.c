/**
 * SPI commands from the SOM
 *
 * Ported from MNT Reform reform2-lpc-fw.
 */

#include "spi_com.h"

void init_spi_client()
{
    gpio_set_function(PIN_SOM_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SOM_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SOM_SS0, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SOM_SCK, GPIO_FUNC_SPI);

    spi_init(spi1, 400 * 1000);
    // we don't appreciate the wording, but it's the API we are given
    spi_set_slave(spi1, true);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);

    printf("# [spi] init_spi_client done\n");
}

void handle_spi_commands(battery_info_s *battery_info)
{
    int command_len = 0;
    bool all_zeroes = true;
    uint8_t spi_buf[SPI_BUF_LEN];
    unsigned char spi_cmd_state = ST_EXPECT_MAGIC;
    unsigned char spi_command = '\0';
    uint8_t spi_arg1 = 0;

    while (spi_is_readable(spi1) && command_len < SPI_BUF_LEN)
    {
        // 0x00 is "repeated tx data"
        spi_read_blocking(spi1, 0x00, &spi_buf[command_len], 1);
        if (spi_buf[command_len] != 0)
        {
            all_zeroes = false;
        }
        command_len++;
    }

    if (command_len == 0)
    {
        return;
    }

    // states:
    // 0   arg1 byte expected
    // 4   command byte expected
    // 6   execute command
    // 7   magic byte expected
    for (uint8_t s = 0; s < command_len; s++)
    {
        if (spi_cmd_state == ST_EXPECT_MAGIC)
        {
            // magic byte found, prevents garbage data
            // in the bus from triggering a command
            if (spi_buf[s] == 0xb5)
            {
                spi_cmd_state = ST_EXPECT_CMD;
            }
        }
        else if (spi_cmd_state == ST_EXPECT_CMD)
        {
            // read command
            spi_command = spi_buf[s];
            spi_cmd_state = ST_EXPECT_DIGIT_0;
        }
        else if (spi_cmd_state == ST_EXPECT_DIGIT_0)
        {
            // read arg1 byte
            spi_arg1 = spi_buf[s];
            spi_cmd_state = ST_EXPECT_RETURN;
        }
    }

    if (spi_cmd_state == ST_EXPECT_MAGIC && !all_zeroes)
    {
        // reset SPI0 block
        // this is a workaround for confusion with
        // software spi from BPI-CM4 where we get
        // bit-shifted bytes

        init_spi_client();
        spi_cmd_state = ST_EXPECT_MAGIC;
        spi_command = 0;
        spi_arg1 = 0;
        return;
    }

    if (spi_cmd_state != ST_EXPECT_RETURN)
    {
        // waiting for more data
        return;
    }

    printf("# [spi] exec: '%c' 0x%02x\n", spi_command, spi_arg1);

    // clear receive buffer, reuse as send buffer
    memset(spi_buf, 0, SPI_BUF_LEN);

    // execute power state command
    if (spi_command == 'p')
    {
        // toggle system power and/or reset imx
        if (spi_arg1 == 1)
        {
            turn_som_power_off();
        }
        if (spi_arg1 == 2)
        {
            turn_som_power_on();
        }
        if (spi_arg1 == 3)
        {
            // TODO
            // reset_som();
        }

        spi_buf[0] = battery_info->som_is_powered;
    }
    // return firmware version and api info
    else if (spi_command == 'f')
    {
        if (spi_arg1 == 0)
        {
            memcpy(spi_buf, FW_STRING1, 8);
        }
        else if (spi_arg1 == 1)
        {
            memcpy(spi_buf, FW_STRING2, 2);
        }
        else
        {
            // if spi_buf size changes, check that both sides can deal with the longer string
            static_assert(sizeof(spi_buf) == 8);
            memset(spi_buf, 0, sizeof(spi_buf));
            strlcpy((char*)spi_buf, MNTRE_FIRMWARE_VERSION, sizeof(spi_buf));
        }
    }
    // execute status query command
    else if (spi_command == 'q')
    {
        uint8_t percentage = (uint8_t)battery_info->charge_percentage;
        int16_t voltsInt = (int16_t)(battery_info->battery_volts * 1000.0);
        int16_t currentInt = (int16_t)(battery_info->battery_amps * 1000.0);

        spi_buf[0] = (uint8_t)voltsInt;
        spi_buf[1] = (uint8_t)(voltsInt >> 8);
        spi_buf[2] = (uint8_t)currentInt;
        spi_buf[3] = (uint8_t)(currentInt >> 8);
        spi_buf[4] = (uint8_t)percentage;
        spi_buf[5] = (uint8_t)0; // TODO "state" not implemented
        spi_buf[6] = (uint8_t)0;
    }
    // get cell voltage
    else if (spi_command == 'v')
    {
        uint16_t volts = 0;
        if (spi_arg1 == 0)
        {
            volts = battery_info->cell1_volts;
            spi_buf[0] = (uint8_t)volts;
            spi_buf[1] = (uint8_t)(volts >> 8);

            volts = battery_info->cell2_volts;
            spi_buf[2] = (uint8_t)volts;
            spi_buf[3] = (uint8_t)(volts >> 8);
        }
    }
    // get calculated capacity
    else if (spi_command == 'c')
    {
        uint16_t cap_accu = (uint16_t) BATTERY_CAPACITY_MILLIAMP_HOURS * (((float)battery_info->charge_percentage) / 100.0);
        uint16_t cap_min = (uint16_t)0;
        uint16_t cap_max = (uint16_t) BATTERY_CAPACITY_MILLIAMP_HOURS;

        spi_buf[0] = (uint8_t)cap_accu;
        spi_buf[1] = (uint8_t)(cap_accu >> 8);
        spi_buf[2] = (uint8_t)cap_min;
        spi_buf[3] = (uint8_t)(cap_min >> 8);
        spi_buf[4] = (uint8_t)cap_max;
        spi_buf[5] = (uint8_t)(cap_max >> 8);
    }
    else if (spi_command == 'u')
    {
        // not implemented
    }
    else if (spi_command == 'b')
    {
        // only for display v2
        int brightness = spi_arg1;
        // 80% is a limit of the hardware (above, the backlight can flicker)
        if (brightness < 0)
            brightness = 0;
        if (brightness > 80)
            brightness = 80;
        set_display_backlight(brightness);
    }

    // FIXME: if we don't reset, SPI wants to transact the amount of bytes
    // that we read above for unknown reasons
    init_spi_client();

    if (battery_info->som_is_powered)
    {
        spi_write_blocking(spi1, spi_buf, SPI_BUF_LEN);
    }

    spi_cmd_state = ST_EXPECT_MAGIC;
    spi_command = 0;
    spi_arg1 = 0;

    return;
}
