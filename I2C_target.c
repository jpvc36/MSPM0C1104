#include "ti_msp_dl_config.h"

uint8_t gTxCount = 0;
volatile uint8_t gData[8] = {0,0xff,0xff,0xff,0,0,0,0,};
volatile uint8_t IR_State = 0, IR_Count = 0;
volatile uint16_t IR_Data_RC5 = 0, captured = 0, last_capture = 0, pulse_width = 0;
volatile uint32_t IR_Data_SIRC = 0, IR_Data_NEC = 0, NEC_DECODED = 0;
volatile uint16_t gAdcResult;

enum IR_State {
    IR_IDLE,
    IR_START,
    IR_SIRC,
    IR_NEC_DATA,
    IR_RC5_START_0,
    IR_RC5_MID_0,
    IR_RC5_MID_1,
};

void RPi_wakePulse(void);

int main(void)
{
    SYSCFG_DL_init();
    const uint8_t addressLookupTable[] = { 0x77, 0x76, 0x75, 0x74, 0x73, 0x72, 0x71, 0x70 };
    const uint16_t thresholds[] = { 300, 370, 430, 500, 620, 750, 950 };
    DL_ADC12_startConversion(ADC12_0_INST);
    DL_Common_delayCycles(1024);
    gAdcResult = DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_0);
    uint8_t i2cAddress = 0x77;
    for (int i = 6; i >= 0; i--) {
        if (gAdcResult > thresholds[i]) {
            i2cAddress = addressLookupTable[i + 1];
            break;
        }
    }
    DL_I2C_setTargetOwnAddress(I2C_INST, i2cAddress);

    DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);

    NVIC_EnableIRQ(I2C_INST_INT_IRQN);
    NVIC_EnableIRQ(GPIO_BUTTONS_INT_IRQN);
    NVIC_EnableIRQ(QEI_0_INST_INT_IRQN);
    NVIC_EnableIRQ(TIMER_0_INST_INT_IRQN);
    NVIC_EnableIRQ(CAPTURE_0_INST_INT_IRQN);
    DL_Timer_enableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC0_DN_EVENT);
    DL_Timer_startCounter(CAPTURE_0_INST);

    DL_GPIO_disableOutput(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
    DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
    DL_SYSCTL_setSYSOSCFreq(DL_SYSCTL_SYSOSC_FREQ_4M);
    DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);

    DL_SYSCTL_enableSleepOnExit();

    while (1) {
        __WFI();
    }
}

void I2C_INST_IRQHandler(void)
{
    switch (DL_I2C_getPendingInterrupt(I2C_INST)) {

        case DL_I2C_IIDX_TARGET_START:
            gTxCount = 0;
            DL_I2C_flushTargetTXFIFO(I2C_INST);
            break;

        case DL_I2C_IIDX_TARGET_TXFIFO_TRIGGER:
            if (gTxCount < 8) {
                gTxCount += DL_I2C_fillTargetTXFIFO(I2C_INST, (const uint8_t *)&gData[gTxCount], 8 - gTxCount);
            } else while (DL_I2C_transmitTargetDataCheck(I2C_INST, 0x00) != false) ;
            break;

        case DL_I2C_IIDX_TARGET_STOP:
            gData[0] &= 0x1f;
            DL_GPIO_disableOutput(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
            DL_GPIO_togglePins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
            break;

       default:
            break;
    }
}

void QEI_0_INST_IRQHandler(void)
{
    gData[0] |= DL_Timer_getQEIDirection(QEI_0_INST) ? 0xa0 : 0xc0;
    DL_GPIO_enableOutput(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
    DL_Timer_setCaptureCompareValue(CAPTURE_0_INST,
        (DL_Timer_getTimerCount(CAPTURE_0_INST) - 60000),
        DL_TIMER_CC_5_INDEX);
    DL_Timer_clearInterruptStatus(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC5_DN_EVENT);
    DL_Timer_enableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC5_DN_EVENT);
}

void GPIOA_IRQHandler(void) {
    gData[0] |= 0x80;
    gData[7] |= 0x01;
    DL_GPIO_enableOutput(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
    NVIC_DisableIRQ(GPIO_BUTTONS_INT_IRQN);
    DL_Timer_startCounter(TIMER_0_INST);
    NVIC_EnableIRQ(TIMER_0_INST_INT_IRQN);
}

void TIMER_0_INST_IRQHandler(void) {
    if (DL_GPIO_readPins(GPIO_BUTTONS_PORT, GPIO_BUTTONS_ROTARY_SWITCH_PIN)) {
        gData[0] &= 0xe0;
        gData[7] &= 0xfe;
        DL_Timer_stopCounter(TIMER_0_INST);
        NVIC_DisableIRQ(TIMER_0_INST_INT_IRQN);
        NVIC_EnableIRQ(GPIO_BUTTONS_INT_IRQN);
        DL_Timer_setCaptureCompareValue(CAPTURE_0_INST,
            (DL_Timer_getTimerCount(CAPTURE_0_INST) - 60000),
            DL_TIMER_CC_1_INDEX);
        DL_Timer_clearInterruptStatus(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC1_DN_EVENT);
        DL_Timer_enableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC1_DN_EVENT);
    } else {
        if ((gData[0] & 0x1f) < 31) gData[0]++;
        else if (gData[0] & 0x80) RPi_wakePulse();          // pulse only if i2c not active
        gData[0] |= 0x80;
    }
    DL_GPIO_enableOutput(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
    DL_GPIO_togglePins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
}

void CAPTURE_0_INST_IRQHandler(void) {

    uint8_t irqStatus = DL_Timer_getPendingInterrupt(CAPTURE_0_INST);
    switch (irqStatus) {

        case DL_TIMER_IIDX_CC0_DN:
        last_capture = captured;
        captured = DL_Timer_getCaptureCompareValue(CAPTURE_0_INST, DL_TIMER_CC_0_INDEX);
        pulse_width = last_capture - captured;
        // set long timeout:
        DL_Timer_setCaptureCompareValue(
            CAPTURE_0_INST,
            (captured - 60000),
            DL_TIMER_CC_5_INDEX);

            switch (IR_State) {
                case IR_IDLE:
                    // start long timeout:
                    DL_Timer_clearInterruptStatus(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC5_DN_EVENT);
                    DL_Timer_enableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC5_DN_EVENT);
                    IR_State = IR_START;
                    break;

                case IR_START:
                    IR_Data_SIRC = IR_Data_RC5 = IR_Count = 0;
                    // start long timeout:
                    DL_Timer_clearInterruptStatus(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC5_DN_EVENT);
                    DL_Timer_enableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC5_DN_EVENT);
                    if (pulse_width > 1500 / 2 && pulse_width < 2100 / 2) {                 // 1.78 ms
                        IR_Data_RC5 = 0x2000;
                        IR_Count = 2;               // First start bit
                        // short timeout for RC5:
                        DL_Timer_setCaptureCompareValue(CAPTURE_0_INST, (captured - 2000), 2);
                        DL_Timer_clearInterruptStatus(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC2_DN_EVENT);
                        DL_Timer_enableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC2_DN_EVENT);
                        IR_State = IR_RC5_MID_1;    // RC5 start
                    } else if (pulse_width > 2800 / 2 && pulse_width < 3200 / 2) {          // 3.0 ms
                        IR_State = IR_SIRC;         // SIRC start
                    } else if (pulse_width > 10125 / 2 && pulse_width < 12375 / 2) {        // 11.25 ms
                        // short timeout for NEC: 
                        DL_Timer_setCaptureCompareValue(CAPTURE_0_INST, (captured - 1500), 4);
                        DL_Timer_clearInterruptStatus(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC4_DN_EVENT);
                        DL_Timer_enableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC4_DN_EVENT);
                        IR_State = IR_IDLE;         // NEC repeat
                    } else if (pulse_width > 12375 / 2 && pulse_width < 14625 / 2) {        // 13.5 ms
                        IR_Data_NEC = 0;
                        IR_State = IR_NEC_DATA;     // NEC start
                    } else IR_State = IR_IDLE;
                        break;

                case IR_RC5_MID_1:
                    if (pulse_width > 1600 / 2 && pulse_width < 2000 / 2) {
                        IR_Data_RC5 |= (1 << (14 - IR_Count++));        // emit 1
                    } else if (pulse_width > 2500 / 2 && pulse_width < 2900 / 2) {
                        IR_Data_RC5 |= (1 << (14 - IR_Count++));        // emit 1
                        IR_State = IR_RC5_MID_0;
                    } else if (pulse_width > 3400 / 2 && pulse_width < 3800 / 2) {
                        IR_Data_RC5 |= (1 << (14 - IR_Count++));        // emit 1
                        IR_Count++;                                     // emit 0
                    }
                    // start short timeout:
                    DL_Timer_setCaptureCompareValue(CAPTURE_0_INST, (captured - 2000), 2);
                    break;

                case IR_RC5_MID_0:
                    if (pulse_width > 1600 / 2 && pulse_width < 2000 / 2) {
                        IR_Count++;                                     // emit 0
                    } else if (pulse_width > 2500 / 2 && pulse_width < 2900 / 2) {
                        IR_Count++;                                     // emit 0
                        IR_Data_RC5 |= (1 << (13 - IR_Count++));        // emit 1
                        IR_State = IR_RC5_START_0;
                    }
                    // start short timeout:
                    DL_Timer_setCaptureCompareValue(CAPTURE_0_INST, (captured - 2000), 2);
                    break;

                case IR_RC5_START_0:
                    if (pulse_width > 1600 / 2 && pulse_width < 2000 / 2) {
                        IR_Data_RC5 |= (1 << (13 - IR_Count++));        // emit 1
                    } else if (pulse_width > 2500 / 2 && pulse_width < 2900 / 2) {
                        IR_Count ++;                                    // emit 0  
                        IR_State = IR_RC5_MID_0;
                    } else if (pulse_width > 3400 / 2 && pulse_width < 3800 / 2) {
                        IR_Count++;                                     // emit 0
                        IR_Data_RC5 |= (1 << (13 - IR_Count++));        // emit 1
                        IR_State = IR_RC5_MID_1;
                    }
                    // start short timeout:
                   DL_Timer_setCaptureCompareValue(CAPTURE_0_INST, (captured - 2000), 2);
                    break;

                case IR_SIRC:                                           // SIRC bit collection
                    // short timeout for SIRC:
                    DL_Timer_setCaptureCompareValue(CAPTURE_0_INST, (captured - 1500), 3);
                    DL_Timer_clearInterruptStatus(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC3_DN_EVENT);
                    DL_Timer_enableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC3_DN_EVENT);
                    if (pulse_width > 900 / 2 && pulse_width < 2100 / 2) {
                        if (pulse_width > 1500 / 2) {
                            IR_Data_SIRC |= (1 << IR_Count);            // 1-bit
                        }
                    IR_Count++;
                    } else {
                        IR_Count = 0;
                        IR_State = IR_IDLE;                             // Invalid pulse
                    }
                    break;
                case IR_NEC_DATA:                                       // NEC bit collection
                    // short timeout for NEC:
                    DL_Timer_setCaptureCompareValue(CAPTURE_0_INST, captured - 1500, 4);
                    DL_Timer_clearInterruptStatus(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC4_DN_EVENT);
                    DL_Timer_enableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC4_DN_EVENT);
                    if (pulse_width > 875 / 2 && pulse_width < 1375 / 2)
                        IR_Count++;
                    else if (pulse_width > 2000 / 2 && pulse_width < 2500 / 2)
                        IR_Data_NEC |= (1 << IR_Count++);
                    else {
                        IR_Count = 0;
                        IR_State = IR_IDLE;                             // Invalid pulse
                    }
                    break;
                default:
                    IR_Count = 0;
                    IR_State = IR_IDLE;
                    break;
            }
            break;

        case DL_TIMER_IIDX_CC1_DN:                                      // Restore idle state
            DL_GPIO_togglePins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
            DL_GPIO_disableOutput(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
            gData[0] = 0x00;
            DL_Timer_disableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC1_DN_EVENT);
            IR_Count = 0;
            IR_State = IR_IDLE;
            break;

        case DL_TIMER_IIDX_CC2_DN:                                      // RC5 data processing
            DL_GPIO_togglePins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
            gData[3] = IR_Data_RC5 & 0x3f;
            gData[2] = (IR_Data_RC5 >> 6) & 0x1f;
            gData[1] = 0x00;
            if ((gData[0] & 0x1f) < 13) gData[0]++;                     // 13*114 ms = 1482 ms
            else if (gData[0] & 0x80) RPi_wakePulse();                  // pulse only if i2c not active
            gData[0] |= 0x80;
            DL_GPIO_enableOutput(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
            DL_Timer_disableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC2_DN_EVENT);
            IR_Count = 0;
            IR_State = IR_IDLE;
            break;

        case DL_TIMER_IIDX_CC3_DN:                                      // SIRC data processing
            DL_GPIO_togglePins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
            gData[3] = IR_Data_SIRC & 0x7f;
            gData[2] = (IR_Data_SIRC >> 12) & 0xff;
            gData[1] = (IR_Data_SIRC >> 7) & 0x1f;
            if ((gData[0] & 0x1f) < 31) gData[0]++;                     // 31*45 ms = 1385 ms
            else if (gData[0] & 0x80) RPi_wakePulse();                  // pulse only if i2c not active
            gData[0] |= 0x80;
            DL_GPIO_enableOutput(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
            DL_Timer_disableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC3_DN_EVENT);
            IR_Count = 0;
            IR_State = IR_IDLE;
            break;

        case DL_TIMER_IIDX_CC4_DN:                                      // NEC data processing
            DL_GPIO_togglePins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
            if ((IR_Data_NEC & 0x00ff00ff) + ((IR_Data_NEC & 0xff00ff00) >> 8) == 0x00ff00ff)       // nec
                NEC_DECODED = ((IR_Data_NEC & 0x00ff0000) >> 16) + ((IR_Data_NEC & 0x000000ff) << 8);
            else if ((IR_Data_NEC & 0x00ff0000) + ((IR_Data_NEC & 0xff000000) >> 8) == 0x00ff0000)  // necx
                NEC_DECODED = ((IR_Data_NEC & 0x00ff0000) >> 16) + ((IR_Data_NEC & 0x0000ffff) << 8);
            else NEC_DECODED = 0;                                                                   // error
            gData[3] = NEC_DECODED & 0xff;
            gData[2] = (NEC_DECODED >> 8) & 0xff;
            gData[1] = (NEC_DECODED >> 16) & 0xff;
            if ((gData[0] & 0x1f) < 14) gData[0]++;                     // 14*110 ms = 1540 ms
            else if (gData[0] & 0x80) RPi_wakePulse();                  // pulse only if i2c not active
            gData[0] |= 0x80;
            DL_GPIO_enableOutput(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
            DL_Timer_disableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC4_DN_EVENT);
            IR_Count = 0;
            IR_State = IR_IDLE;
            break;

        case DL_TIMER_IIDX_CC5_DN:                                      // IR & QEI data reset
            DL_GPIO_togglePins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
            gData[1] = gData[2] = gData[3] = 0xff;
            gData[0] = 0x80;
            DL_Timer_disableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC5_DN_EVENT);
            DL_GPIO_enableOutput(GPIO_LEDS_PORT, GPIO_LEDS_IRQ_PIN);
            DL_Timer_setCaptureCompareValue(CAPTURE_0_INST, (captured - 60000), 1);
            DL_Timer_clearInterruptStatus(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC1_DN_EVENT);
            DL_Timer_enableInterrupt(CAPTURE_0_INST, DL_TIMER_INTERRUPT_CC1_DN_EVENT);
            IR_Count = 0;
            IR_State = IR_IDLE;
            break;
        default:
            IR_Count = 0;
            IR_State = IR_IDLE;
            break;
    }
}

void RPi_wakePulse(void) {
    DL_I2C_disableTarget(I2C_INST);
    DL_GPIO_initPeripheralInputFunction(IOMUX_PINCM2, IOMUX_PINCM2_PF_GPIOA_DIO01);
    DL_GPIO_enableOutput(GPIOA, DL_GPIO_PIN_1);

    DL_GPIO_setPins(GPIOA, DL_GPIO_PIN_1);                              // idle high
    DL_GPIO_clearPins(GPIOA, DL_GPIO_PIN_1);                            // pulse low
    DL_Common_delayCycles(40000);                               // ~10 ms @ 4 MHz
    DL_GPIO_setPins(GPIOA, DL_GPIO_PIN_1);                              // back to high

    DL_GPIO_initPeripheralInputFunction(IOMUX_PINCM2, IOMUX_PINCM2_PF_I2C0_SCL);
    DL_I2C_enableTarget(I2C_INST);
}

