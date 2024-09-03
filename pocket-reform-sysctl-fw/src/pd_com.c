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
    (void)battery_info;
    pd_state->ticks++;

    if (pd_state->ticks > 300)
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

    if (pd_state->state == PD_STATE_RESET_IDLE)
    {
        printf("# [pd] resetting pd port\n");
        gpio_put(PIN_LED_R, 0);
        
        // turn off vbus power
        gpio_put(PIN_USB_SRC_ENABLE, 0);
        sleep_us(10);

        // reset
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
            pd_state->next_state = PD_STATE_RESET_IDLE;
        }
    }
    else if (pd_state->state == PD_STATE_USB_DETECT)
    {
        printf("# [pd] trying usb detection\n");

        // is VBUS already powered?
        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_PU_EN2 | FUSB_SWITCHES0_PU_EN1);
        fusb_write_byte(FUSB_MEASURE, (FUSB_MEASURE_MEAS_VBUS) | 0b110001);
        uint8_t vbus_ok = (fusb_read_byte(FUSB_STATUS0) & FUSB_STATUS0_VBUSOK) > 0;
        printf("# [pd] vbusok: %d\n", vbus_ok);

        if (vbus_ok)
        {
            // must actually be PD, switch to charger detect.
            printf("# [pd] [ERROR] toggle wrong: %d\n", vbus_ok);
            pd_state->next_state = PD_STATE_CHARGER_DETECT;
        }
        fusb_write_byte(FUSB_MEASURE, 0b110001);

        sleep_us(10);

        /**
            Host software utilizes I_COMP and
            I_BC_LVL interrupts to determine an
            attach

            FUSB302B I_COMP interrupt alerts
            host software that a detach has
            occurred
        */

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
            pd_state->next_state = PD_STATE_CHARGER_DETECT;
        }

        // BC_LVL
        // Current voltage status of the measured CC pin interpreted as host current levels as follows

        // COMP
        // Measured CC* input is higher than reference level driven from the MDAC

        /*
            Host software configures
            FUSB302B based on insertion
            orientation and enables VBUS
            and VCONN
         */
    }
    else if (pd_state->state == PD_STATE_CHARGER_DETECT)
    {
        printf("# [pd] trying charger \n");

        fusb_write_byte(FUSB_SWITCHES0, FUSB_SWITCHES0_PDWN_2 | FUSB_SWITCHES0_PDWN_1);

        // Measure VBUS
        uint8_t vbus_ok = 0;

        // wait a bit for VBUS to become powered
        for(int s = 0; s < 10; s++)
        {
            fusb_write_byte(FUSB_MEASURE, (FUSB_MEASURE_MEAS_VBUS) | 0b110001);
            vbus_ok = (fusb_read_byte(FUSB_STATUS0) & FUSB_STATUS0_VBUSOK) > 0;
            printf("# [pd] vbusok: %d\n", vbus_ok);

            if(vbus_ok > 0)
            {
                break;
            }
            sleep_ms(100);
        }
        fusb_write_byte(FUSB_MEASURE, 0b110001);

        if (!vbus_ok)
        {
            printf("# [pd] [WARN]: charger not up, vbus %d\n", vbus_ok);
            pd_state->next_state = PD_STATE_RESET_IDLE;
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


        fusb_write_byte(FUSB_CONTROL0, FUSB_CONTROL0_TX_FLUSH);
        fusb_write_byte(FUSB_CONTROL1, FUSB_CONTROL1_RX_FLUSH);
        fusb_write_byte(FUSB_RESET, FUSB_RESET_PD_RESET);

        //TODO: wait some number of times before giving up
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
            printf("# [pd] no response, resetting\n");
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
                        pd_state->power_object_index = i + 1;
                        max_voltage = voltage;
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

            // FIXME: incorrect - charger config and requested amps should match
            // update charger based on amps that input can provide?

            int amps_pdi = PD_MA2PDI(1500); // PDI amps are units of 10 MA

            tx.obj[0] = PD_RDO_FV_MAX_CURRENT_SET(amps_pdi) | PD_RDO_FV_CURRENT_SET(amps_pdi) | PD_RDO_NO_USB_SUSPEND | PD_RDO_OBJPOS_SET(pd_state->power_object_index);

            fusb_send_message(&tx);

            printf("# [pd] request sent\n");

            pd_state->tx_id_count++;

            pd_state->request_sent = true;
        }
        else
        {
            printf("# [pd] msg type: 0x%x numobj: %d\n", msgtype, numobj);
        }

        for (int s = 0; s < 20; s++)
        {
            int res = fusb_read_message(&rx_msg);

            if (!res)
            {
                printf("# [pd] charger responded\n");

                uint8_t msgtype = PD_MSGTYPE_GET(&rx_msg);
                if (msgtype == PD_MSGTYPE_PS_RDY)
                {
                    // power supply is ready
                    printf("# [pd] power supply ready\n");
                    pd_state->next_state = PD_STATE_CHARGER_POWERED;
                    return;
                }
                else
                {
                    printf("# [pd] unexpected message %d\n", msgtype);
                }
            }
            sleep_ms(100);
        }

        printf("# [pd] [ERROR] no response from charger\n");
    }
    else if (pd_state->state == PD_STATE_CHARGER_POWERED)
    {
        gpio_put(PIN_LED_R, 1);

        if (battery_info->input_volts != -1)
        {
            printf("# [pd] input_voltage: %.2fv\n", battery_info->input_volts);
            if (battery_info->input_volts < 6)
            {
                printf("# [pd] input voltage below 6v, unplugged or charger failed to power\n");
                pd_state->next_state = PD_STATE_RESET_IDLE;
            }
        }
        else
        {
            printf("# [pd] charger not available, max17320_devname: %x\n", battery_info->max17320_devname);
        }
    }
}