#include "pd_com.h"

void print_src_fixed_pdo(int number, uint32_t pdo)
{
    int tmp;

    printf("[pd_src_fixed_pdo]\n");
    printf("\tnumber = %d\n", number);

    /* Dual-role power */
    tmp = (pdo & PD_PDO_SRC_FIXED_DUAL_ROLE_PWR) >> PD_PDO_SRC_FIXED_DUAL_ROLE_PWR_SHIFT;
    if (tmp)
    {
        printf("\tdual_role_pwr = %d\n", tmp);
    }

    /* USB Suspend Supported */
    tmp = (pdo & PD_PDO_SRC_FIXED_USB_SUSPEND) >> PD_PDO_SRC_FIXED_USB_SUSPEND_SHIFT;
    if (tmp)
    {
        printf("\tusb_suspend = %d\n", tmp);
    }

    /* Unconstrained Power */
    tmp = (pdo & PD_PDO_SRC_FIXED_UNCONSTRAINED) >> PD_PDO_SRC_FIXED_UNCONSTRAINED_SHIFT;
    if (tmp)
    {
        printf("\tunconstrained_pwr = %d\n", tmp);
    }

    /* USB Communications Capable */
    tmp = (pdo & PD_PDO_SRC_FIXED_USB_COMMS) >> PD_PDO_SRC_FIXED_USB_COMMS_SHIFT;
    if (tmp)
    {
        printf("\tusb_comms = %d\n", tmp);
    }

    /* Dual-Role Data */
    tmp = (pdo & PD_PDO_SRC_FIXED_DUAL_ROLE_DATA) >> PD_PDO_SRC_FIXED_DUAL_ROLE_DATA_SHIFT;
    if (tmp)
    {
        printf("\tdual_role_data = %d\n", tmp);
    }

    /* Unchunked Extended Messages Supported */
    tmp = (pdo & PD_PDO_SRC_FIXED_UNCHUNKED_EXT_MSG) >> PD_PDO_SRC_FIXED_UNCHUNKED_EXT_MSG_SHIFT;
    if (tmp)
    {
        printf("\tunchunked_ext_msg = %d\n", tmp);
    }

    /* Peak Current */
    tmp = (pdo & PD_PDO_SRC_FIXED_PEAK_CURRENT) >> PD_PDO_SRC_FIXED_PEAK_CURRENT_SHIFT;
    if (tmp)
    {
        printf("\tpeak_i = %d\n", tmp);
    }

    /* Voltage */
    tmp = (pdo & PD_PDO_SRC_FIXED_VOLTAGE) >> PD_PDO_SRC_FIXED_VOLTAGE_SHIFT;
    printf("\tv = %d.%02d\n", PD_PDV_V(tmp), PD_PDV_CV(tmp));

    /* Maximum Current */
    tmp = (pdo & PD_PDO_SRC_FIXED_CURRENT) >> PD_PDO_SRC_FIXED_CURRENT_SHIFT;
    printf("\ti_a: %d.%02d\n", PD_PDI_A(tmp), PD_PDI_CA(tmp));
}

void handle_pd_state(battery_info_s *battery_info, pd_state_s *pd_state)
{
    pd_state->ticks++;

    // enable a state evaluation every 1000 ms
    if (pd_state->ticks > 100)
    {
        pd_state->ticks = 0;
    }
    else
    {
        return;
    }

    if (pd_state->next_state != pd_state->state)
    {
        printf("# [pd] new state: %d\n", pd_state->next_state);
        pd_state->state = pd_state->next_state;
    }

    if (pd_state->state == PD_STATE_RESET)
    {
        printf("# [pd] resetting pd port\n");
        gpio_put(PIN_LED_R, 0);

        // turn off vbus power
        gpio_put(PIN_USB_SRC_ENABLE, 0);
        sleep_us(10);

        // soft reset
        fusb_write_byte(FUSB_RESET, FUSB_RESET_SW_RES);
        sleep_us(10);

        // enable dac for measuring CC lines
        fusb_write_byte(FUSB_POWER, 0x0F);

        // default pull down resistors
        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_PDWN_2 | FUSB_SWITCHES0_PDWN_1);

        pd_state->next_state = PD_STATE_USB_DETECT;
    }
    else if (pd_state->state == PD_STATE_USB_ATTACHED)
    {
        // Measure CC1
        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_MEAS_CC1 | FUSB_SWITCHES0_PU_EN2 | FUSB_SWITCHES0_PU_EN1); //  MEAS_CC1|PDWN2   |PDWN1
        sleep_us(250);
        uint8_t cc1 = fusb_read_byte(FUSB_STATUS0) & FUSB_STATUS0_BC_LVL;

        // Measure CC2
        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_MEAS_CC2 | FUSB_SWITCHES0_PU_EN2 | FUSB_SWITCHES0_PU_EN1); //  MEAS_CC2|PDWN2   |PDWN1
        sleep_us(250);
        uint8_t cc2 = fusb_read_byte(FUSB_STATUS0) & FUSB_STATUS0_BC_LVL;

        printf("# [pd] VBUS Powered, CC1: %d CC2: %d\n", cc1, cc2);

        if (cc1 == cc2)
        {
            printf("# [pd] invalid CC state \n");
            pd_state->next_state = PD_STATE_RESET;
        }
    }
    else if (pd_state->state == PD_STATE_USB_DETECT)
    {
        /**
         * 1. if VBUS is already powered than this has to be a charger
         * 2. enable pullups and test CC lines to see if there is a device attached
         * 3. if CC lines are valid, power VBUS and
         */
        printf("# [pd] searching for usb device\n");

        // is VBUS already powered?
        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_PU_EN2 | FUSB_SWITCHES0_PU_EN1);
        fusb_write_byte(FUSB_MEASURE, (FUSB_MEASURE_MEAS_VBUS) | 0b110001);
        uint8_t vbus_ok = (fusb_read_byte(FUSB_STATUS0) & FUSB_STATUS0_VBUSOK) > 0;
        printf("# [pd] vbusok: %d\n", vbus_ok);

        if (vbus_ok)
        {
            // must actually be PD device, switch to charger detect.
            printf("# [pd] [ERROR] toggle wrong: %d\n", vbus_ok);
            pd_state->next_state = PD_STATE_CHARGER_DETECT;
        }
        fusb_write_byte(FUSB_MEASURE, 0b110001);
        sleep_us(250);

        // Measure CC1
        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_MEAS_CC1 | FUSB_SWITCHES0_PU_EN2 | FUSB_SWITCHES0_PU_EN1); //  MEAS_CC1|PDWN2   |PDWN1
        sleep_us(250);
        uint8_t cc1 = fusb_read_byte(FUSB_STATUS0) & FUSB_STATUS0_BC_LVL;

        // Measure CC2
        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_MEAS_CC2 | FUSB_SWITCHES0_PU_EN2 | FUSB_SWITCHES0_PU_EN1); //  MEAS_CC2|PDWN2   |PDWN1
        sleep_us(250);
        uint8_t cc2 = fusb_read_byte(FUSB_STATUS0) & FUSB_STATUS0_BC_LVL;

        printf("# [pd] CC1: %d CC2: %d\n", cc1, cc2);

        if (cc1 != cc2 && cc1 > 0 && cc2 > 0)
        {
            fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_PU_EN2 | FUSB_SWITCHES0_PU_EN1);
            printf("# [pd] USB Device Detected, enabling VBUS\n");
            gpio_put(PIN_USB_SRC_ENABLE, 1);
            pd_state->next_state = PD_STATE_USB_ATTACHED;
        }
        else
        {
            printf("# [pd] invalid CC state \n");
            pd_state->next_state = PD_STATE_CHARGER_VBUS_DETECT;
        }
    }
    else if (pd_state->state == PD_STATE_CHARGER_VBUS_DETECT)
    {
        /**
         * set pulldowns and measure vbus, pd chargers must send 5 volts
         * it might take a second before the charger sends power in response to the CC pull downs being set
         *    -> on fail try USB detection
         */
        pd_state->cycles_in_charger_vbus_detect++;
        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_PDWN_2 | FUSB_SWITCHES0_PDWN_1);

        // measure VBUS
        uint8_t vbus_ok = 0;

        // wait for VBUS to become powered
        fusb_write_byte(FUSB_MEASURE, (FUSB_MEASURE_MEAS_VBUS) | 0b110001);
        vbus_ok = (fusb_read_byte(FUSB_STATUS0) & FUSB_STATUS0_VBUSOK) > 0;

        if (vbus_ok > 0)
        {
            pd_state->next_state = PD_STATE_CHARGER_DETECT;
        }

        fusb_write_byte(FUSB_MEASURE, 0b110001);

        if (!vbus_ok && pd_state->cycles_in_charger_vbus_detect > 5)
        {
            printf("# [pd] charger not up, vbus %d\n", vbus_ok);
            pd_state->cycles_in_charger_vbus_detect = 0;
            pd_state->next_state = PD_STATE_USB_DETECT;
            return;
        }
    }
    else if (pd_state->state == PD_STATE_CHARGER_DETECT)
    {
        /**
         * 1. try to reach the fusb302b
         *    -> on fail reset
         * 3. measure CC lines to determine orientation
         *    -> on fail try USB detection
         * 4. flush and reset pd controller
         * 5. wait for capabilities message
         *    -> no message, retry charger detect state
         * 6. send power request
         * 7. wait for ready message
         *    -> no message, retry charger detect state
         *    -> ready response, go to charger powered state
         */
        printf("# [pd] searching for pd charger\n");

        // probe FUSB302BMPX
        uint8_t rxdata[2];
        if (!i2c_read_timeout_us(i2c0, FUSB_ADDR, rxdata, 1, false, I2C_TIMEOUT))
        {
            printf("# [pd] [ERROR] fusb302b not reachable\n");
            // not reachable, skip any charger logic
            pd_state->next_state = PD_STATE_RESET;
            return;
        }

        fusb_write_byte(FUSB_CONTROL3,
                        FUSB_CONTROL3_AUTO_HARDRESET |
                            FUSB_CONTROL3_AUTO_SOFTRESET |
                            (3 << FUSB_CONTROL3_N_RETRIES_SHIFT) |
                            FUSB_CONTROL3_AUTO_RETRY);

        // Measure CC1
        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_MEAS_CC1 | FUSB_SWITCHES0_PDWN_2 | FUSB_SWITCHES0_PDWN_1); //  MEAS_CC1|PDWN2   |PDWN1
        sleep_us(250);
        uint8_t cc1 = fusb_read_byte(FUSB_STATUS0) & FUSB_STATUS0_BC_LVL;

        // Measure CC2
        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_MEAS_CC2 | FUSB_SWITCHES0_PDWN_2 | FUSB_SWITCHES0_PDWN_1); //  MEAS_CC2|PDWN2   |PDWN1
        sleep_us(250);
        uint8_t cc2 = fusb_read_byte(FUSB_STATUS0) & FUSB_STATUS0_BC_LVL;

        printf("# [pd] CC1: %d CC2: %d\n", cc1, cc2);

        if (cc1 > cc2)
        {
            fusb_write_byte(FUSB_SWITCHES1, FUSB_SWITCHES1_AUTO_CRC | FUSB_SWITCHES1_TXCC1);
            fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_MEAS_CC1 | FUSB_SWITCHES0_PDWN_2 | FUSB_SWITCHES0_PDWN_1);
        }
        else if (cc1 < cc2)
        {
            fusb_write_byte(FUSB_SWITCHES1, FUSB_SWITCHES1_AUTO_CRC | FUSB_SWITCHES1_TXCC2);
            fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_MEAS_CC2 | FUSB_SWITCHES0_PDWN_2 | FUSB_SWITCHES0_PDWN_1);
        }
        else
        {
            printf("# [pd] invalid CC state\n");
            pd_state->next_state = PD_STATE_USB_DETECT;
            return;
        }

        // flush and reset pd controller
        fusb_write_byte(FUSB_CONTROL0, FUSB_CONTROL0_TX_FLUSH);
        fusb_write_byte(FUSB_CONTROL1, FUSB_CONTROL1_RX_FLUSH);
        fusb_write_byte(FUSB_RESET, FUSB_RESET_PD_RESET);

        // TODO:
        // move to a seperate state for waiting?
        // move to reset state instead?

        union pd_msg rx_msg;
        int res = 0;
        for (int i = 0; i < 10; i++)
        {
            res = fusb_read_message(&rx_msg);
            if (!res)
            {
                break;
            }
            sleep_ms(10);
        }

        if (res)
        {
            // no response from charger, retry state
            printf("# [pd] [ERROR] no response from charger\n");
            return;
        }

        printf("# [pd] charger responded\n");

        uint8_t msgtype = PD_MSGTYPE_GET(&rx_msg);
        uint8_t numobj = PD_NUMOBJ_GET(&rx_msg);
        if (msgtype == PD_MSGTYPE_SOURCE_CAPABILITIES)
        {
            int max_voltage = 0;
            for (int i = 0; i < numobj; i++)
            {
                uint32_t pdo = rx_msg.obj[i];

                if ((pdo & PD_PDO_TYPE) == PD_PDO_TYPE_FIXED)
                {
                    print_src_fixed_pdo(i + 1, pdo);

                    int voltage = (int)PD_PDV_V(PD_PDO_SRC_FIXED_VOLTAGE_GET(pdo));

                    if (voltage > max_voltage && voltage <= 20)
                    {
                        max_voltage = voltage;
                        pd_state->power_object_index = i + 1;
                        pd_state->power_requested_milliamps = PD_PDO_SRC_FIXED_CURRENT_GET(pdo) * 10;
                        pd_state->power_requested_volts = voltage;
                    }
                }
                else
                {
                    printf("# [pd] not a fixed PDO: 0x%08x\n", (uint16_t)pdo);
                }
            }

            printf("# [pd] requesting PO %d\n", pd_state->power_object_index);

            union pd_msg tx;
            tx.hdr = PD_MSGTYPE_REQUEST | PD_NUMOBJ(1) | PD_DATAROLE_UFP | PD_POWERROLE_SINK | PD_SPECREV_2_0;

            tx.hdr &= ~PD_HDR_MESSAGEID;
            tx.hdr |= (pd_state->tx_id_count % 8) << PD_HDR_MESSAGEID_SHIFT;

            // MP2650 charger data sheet says 45 watts max total power input
            // battery charger max wattage is is 4.2v * 2 * 2000 ma (configurable) = 16.8 watts
            // system load max current can be combination of charger input and available battery current
            // FIXME, assume desired wattage of 45 watts

            uint16_t desired_milliamps = 45 * 1000 / pd_state->power_requested_volts;
            uint16_t amps_pdi = PD_MA2PDI(desired_milliamps);

            if (pd_state->power_requested_milliamps < desired_milliamps)
            {
                printf("# [pd] [WARNING] available power role is less than 45 watts\n");
                printf("# [pd] [WARNING] volts: %d want: %d ma found: %d ma\n",
                       pd_state->power_requested_volts,
                       desired_milliamps,
                       pd_state->power_requested_milliamps);

                amps_pdi = PD_MA2PDI(pd_state->power_requested_milliamps);
            }

            // TODO, if power is less than 45 watts set the PD_RDO_CAP_MISMATCH flag on the request
            // I don't know what the charger is supposed to do about that
            tx.obj[0] = PD_RDO_FV_MAX_CURRENT_SET(amps_pdi) | PD_RDO_FV_CURRENT_SET(amps_pdi) | PD_RDO_NO_USB_SUSPEND | PD_RDO_OBJPOS_SET(pd_state->power_object_index);

            fusb_send_message(&tx);

            printf("# [pd] request sent\n");

            pd_state->tx_id_count++;
        }
        else
        {
            printf("# [pd] msg type: 0x%x numobj: %d\n", msgtype, numobj);
        }

        // wait for up to 5 seconds for a response
        for (int i = 0; i < 500; i++)
        {
            res = fusb_read_message(&rx_msg);

            if (!res)
            {
                uint8_t msgtype = PD_MSGTYPE_GET(&rx_msg);
                printf("# [pd] charger responded\n");

                if (msgtype == PD_MSGTYPE_ACCEPT)
                {
                    printf("# [pd] power supply accepted, t:%d\n", i);
                }
                else if (msgtype == PD_MSGTYPE_PS_RDY)
                {
                    printf("# [pd] power supply ready, t:%d\n", i);

                    // force update of battery gauge before going to powered state
                    battery_info->ticks = 1000;

                    pd_state->next_state = PD_STATE_CHARGER_POWERED;
                    return;
                }
                else
                {
                    printf("# [pd] unexpected message %d\n", msgtype);
                }
            }
            else
            {
                sleep_ms(10);
            }
        }

        // retry state
        printf("# [pd] [ERROR] no response from charger\n");
    }
    else if (pd_state->state == PD_STATE_CHARGER_POWERED)
    {
        // Validate with power gauge that pd is sending a voltage over 6 volts
        // TODO: validate that PD charger is sending requested voltage within some tolerance

        if (battery_info->input_volts != -1)
        {
            if (battery_info->input_volts < 6)
            {
                printf("# [pd] input_voltage: %.2fv\n", battery_info->input_volts);
                printf("# [pd] input voltage below 6v, unplugged or charger failed to power\n");
                pd_state->next_state = PD_STATE_RESET;
            }
            else
            {
                gpio_put(PIN_LED_R, 1);
            }
        }
        else
        {
            printf("# [pd] gauge not available, max17320_devname: %x\n", battery_info->max17320_devname);
            pd_state->next_state = PD_STATE_RESET;
        }
    }
}