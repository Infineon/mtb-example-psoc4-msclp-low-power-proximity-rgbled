/******************************************************************************
* File Name:   main.c
*
* Description: This is the source code for the PSoC4 MSCLP CAPSENSE low power
* proximity tuning code example for ModusToolbox.
*
* Related Document: See README.md
*
*
*******************************************************************************
* Copyright 2022-2023, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

/*******************************************************************************
 * Include header files
 ******************************************************************************/
#include "cy_pdl.h"
#include "cybsp.h"
#include "cycfg.h"
#include "cycfg_capsense.h"
#include "user_led_control.h"

/*******************************************************************************
* User configurable Macros
********************************************************************************/
/* Enable this, if Tuner needs to be enabled */
#define ENABLE_TUNER                     (1u)

/* Enable this, if Serial LED needs to be enabled */
#define ENABLE_SPI_SERIAL_LED            (1u)
#define SERIAL_LED_BRIGHTNESS_MAX       (255u)

/* 128Hz Refresh rate in Active mode */
#define ACTIVE_MODE_REFRESH_RATE         (128u)

/* 32Hz Refresh rate in Active-Low Refresh rate(ALR) mode */
#define ALR_MODE_REFRESH_RATE            (32u)

/* Timeout to move from ACTIVE mode to ALR mode if there is no user activity */
#define ACTIVE_MODE_TIMEOUT_SEC          (5u)

/* Timeout to move from ALR mode to WOT mode if there is no user activity */
#define ALR_MODE_TIMEOUT_SEC             (5u)

/* Scan time in microseconds */
#define ACTIVE_MODE_FRAME_SCAN_TIME     (2891u)

/* Active mode Processing time in us ~= 23us with Serial LED and Tuner disabled*/
#define ACTIVE_MODE_PROCESS_TIME        (23u)

/* Scan time in microseconds */
#define ALR_MODE_FRAME_SCAN_TIME        (2891u)

/* ALR mode Processing time in us ~= 23us with Serial LED and Tuner disabled*/
#define ALR_MODE_PROCESS_TIME           (23u)

/* Touch status of the proximity sensor */
#define TOUCH_STATE                     (3u)

/* Proximity status of the proximity sensor */
#define PROX_STATE                      (1u)

/* Enable run time measurements for various modes of the application, 
* this run time is used to calculate MSCLP timer reload value */
#define ENABLE_RUN_TIME_MEASUREMENT      (0u)

/*******************************************************************************
* Macros
********************************************************************************/
#define CAPSENSE_MSC0_INTR_PRIORITY      (3u)

#define CY_ASSERT_FAILED                 (0u)

#define EZI2C_INTR_PRIORITY              (2u)

#define ILO_FREQ                        (40000u)
#define TIME_IN_US                      (1000000u)

#define MINIMUM_TIMER                   (TIME_IN_US / ILO_FREQ)

/* 128Hz Refresh rate in Active mode */
#if ((TIME_IN_US / ACTIVE_MODE_REFRESH_RATE) > (ACTIVE_MODE_FRAME_SCAN_TIME + ACTIVE_MODE_PROCESS_TIME))
    #define ACTIVE_MODE_TIMER           (TIME_IN_US / ACTIVE_MODE_REFRESH_RATE - \
                                        (ACTIVE_MODE_FRAME_SCAN_TIME + ACTIVE_MODE_PROCESS_TIME))
#elif
    #define ACTIVE_MODE_TIMER           (MINIMUM_TIMER)
#endif

#if ((TIME_IN_US / ALR_MODE_REFRESH_RATE) > (ALR_MODE_FRAME_SCAN_TIME + ALR_MODE_PROCESS_TIME))
    #define ALR_MODE_TIMER              (TIME_IN_US / ALR_MODE_REFRESH_RATE - \
                                            (ALR_MODE_FRAME_SCAN_TIME + ALR_MODE_PROCESS_TIME))
#elif
    #define ALR_MODE_TIMER              (MINIMUM_TIMER)
#endif

#define ACTIVE_MODE_TIMEOUT             (ACTIVE_MODE_REFRESH_RATE * ACTIVE_MODE_TIMEOUT_SEC)

#define ALR_MODE_TIMEOUT                (ALR_MODE_REFRESH_RATE * ALR_MODE_TIMEOUT_SEC)

#define TIMEOUT_RESET                   (0u)

#if ENABLE_RUN_TIME_MEASUREMENT
    #define SYS_TICK_MAX_INTERVAL       (0x00FFFFFF)
    #define TIME_PER_TICK_IN_US         ((float)TIME_IN_US/CY_CAPSENSE_CPU_CLK)
#endif

/*****************************************************************************
* Finite state machine states for device operating states
*****************************************************************************/
typedef enum
{
    ACTIVE_MODE = 0x01u,    /* Active mode - All the sensors are scanned in this state
                            * with highest refresh rate */
    ALR_MODE = 0x02u,       /* Active-Low Refresh Rate (ALR) mode - All the sensors are
                            * scanned in this state with low refresh rate */
    WOT_MODE = 0x03u        /* Wake on Touch (WoT) mode - Low Power sensors are scanned
                            * in this state with lowest refresh rate */
} APPLICATION_STATE;

/*******************************************************************************
* Function Prototypes
*******************************************************************************/
static void InitializeCapsense(void);
static void Capsense_Msc0Isr(void);

static void Ezi2cIsr(void);
static void InitializeCapsenseTuner(void);

#if ENABLE_RUN_TIME_MEASUREMENT
static void StartRuntimeMeasurement();
static uint32_t StopRuntimeMeasurement();
#endif

#if ENABLE_SPI_SERIAL_LED
void UpdateLeds(void);
#endif

void RegisterCallback(void);

/* Deep Sleep Callback function */
cy_en_syspm_status_t DeepSleepCallback(cy_stc_syspm_callback_params_t *callbackParams,
                                         cy_en_syspm_callback_mode_t mode);

/*******************************************************************************
* Global Definitions
*******************************************************************************/
/* Variables holds the current low power state [ACTIVE, ALR or WOT] */
APPLICATION_STATE appState;

cy_stc_scb_ezi2c_context_t ezi2cContext;

#if ENABLE_SPI_SERIAL_LED
extern cy_stc_scb_spi_context_t UserSpiContext;
extern serialLedContext_t ledContext;
#endif

/* Callback parameters for custom, EzI2C, SPI */

/* Callback parameters for EzI2C */
cy_stc_syspm_callback_params_t ezi2cCallbackParams =
{
    .base       = SCB1,
    .context    = &ezi2cContext
};

#if ENABLE_SPI_SERIAL_LED
/* Callback parameters for SPI */
cy_stc_syspm_callback_params_t spiCallbackParams =
{
    .base       = SCB0,
    .context    = &UserSpiContext
};
#endif

/* Callback parameters for custom callback */
cy_stc_syspm_callback_params_t deepSleepCallBackParams = {
    .base       =  NULL,
    .context    =  NULL
};

/* Callback declaration for EzI2C Deep Sleep callback */
cy_stc_syspm_callback_t ezi2cCallback =
{
    .callback       = (Cy_SysPmCallback)&Cy_SCB_EZI2C_DeepSleepCallback,
    .type           = CY_SYSPM_DEEPSLEEP,
    .skipMode       = 0UL,
    .callbackParams = &ezi2cCallbackParams,
    .prevItm        = NULL,
    .nextItm        = NULL,
    .order          = 0
};

#if ENABLE_SPI_SERIAL_LED
/* Callback declaration for SPI Deep Sleep callback */
cy_stc_syspm_callback_t spiCallback =
{
    .callback       = (Cy_SysPmCallback)&Cy_SCB_SPI_DeepSleepCallback,
    .type           = CY_SYSPM_DEEPSLEEP,
    .skipMode       = 0UL,
    .callbackParams = &spiCallbackParams,
    .prevItm        = NULL,
    .nextItm        = NULL,
    .order          = 1
};
#endif

/* Callback declaration for Custom Deep Sleep callback */
cy_stc_syspm_callback_t deepSleepCb =
{
    .callback       = &DeepSleepCallback,
    .type           = CY_SYSPM_DEEPSLEEP,
    .skipMode       = 0UL,
    .callbackParams = &deepSleepCallBackParams,
    .prevItm        = NULL,
    .nextItm        = NULL,
    .order          = 2
};

#if ENABLE_RUN_TIME_MEASUREMENT
volatile uint32_t processTime = 0u;
#endif

/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
*  System entrance point. This function performs
*  - initial setup of device
*  - initialize CAPSENSE
*  - initialize tuner communication
*  - scan proximity and touch continuously at 3 different power modes
*  - serial RGB LED for proximity and touch indication
*
* Return:
*  int
*
*******************************************************************************/
int main(void)
{
    cy_rslt_t result;
    uint32_t appStateTimeoutCount;
    uint32_t interruptStatus;

#if ENABLE_RUN_TIME_MEASUREMENT
    static uint32_t activeModeRunTime;
    static uint32_t alrModeRunTime;
#endif

    /* Initialize the device and board peripherals */
    result = cybsp_init() ;

#if ENABLE_RUN_TIME_MEASUREMENT
    Cy_SysTick_Init (CY_SYSTICK_CLOCK_SOURCE_CLK_CPU ,SYS_TICK_MAX_INTERVAL);
#endif

    /* Board init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(CY_ASSERT_FAILED);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Initialize EZI2C */
    InitializeCapsenseTuner();

#if ENABLE_SPI_SERIAL_LED
    /* Initialize SPI master */
    result = InitSpiMaster();
    /* Initialization failed. Stop program execution */
    if(result != INIT_SUCCESS)
    {
        CY_ASSERT(0);
    }
#else
    /* SPI pins drive mode to Analog HighZ */
    Cy_GPIO_SetDrivemode(CYBSP_SERIAL_LED_PORT, CYBSP_SERIAL_LED_NUM, CY_GPIO_DM_ANALOG);
#endif

    /* Register callbacks */
    RegisterCallback();

    /* Define initial state of the device and the corresponding refresh rate*/
    appState = ACTIVE_MODE;
    appStateTimeoutCount = 0u;

    /* Initialize MSC CAPSENSE */
    InitializeCapsense();

#if ENABLE_SPI_SERIAL_LED
    /* Serial LED control for showing the CAPSENSE touch status and power mode */
    UpdateLeds();
#endif

    /* Measures the actual ILO frequency and compensate MSCLP wake up timers */
    Cy_CapSense_IloCompensate(&cy_capsense_context);

    /* Configure the MSCLP wake up timer as per the ACTIVE mode refresh rate */
    Cy_CapSense_ConfigureMsclpTimer(ACTIVE_MODE_TIMER, &cy_capsense_context);


    for (;;)
    {
        switch(appState)
        {
            /* Active Refresh-rate Mode */
            case ACTIVE_MODE:

                Cy_CapSense_ScanAllSlots(&cy_capsense_context);

                interruptStatus = Cy_SysLib_EnterCriticalSection();

                while (Cy_CapSense_IsBusy(&cy_capsense_context))
                {
                    Cy_SysPm_CpuEnterDeepSleep();

                    Cy_SysLib_ExitCriticalSection(interruptStatus);
                    interruptStatus = Cy_SysLib_EnterCriticalSection();
                }
                Cy_SysLib_ExitCriticalSection(interruptStatus);

#if ENABLE_RUN_TIME_MEASUREMENT
                activeModeRunTime = 0u;
                StartRuntimeMeasurement();
#endif
                Cy_CapSense_ProcessAllWidgets(&cy_capsense_context);

                /* Scan, process and check the status of the all Active mode sensors */
                if(Cy_CapSense_IsAnyWidgetActive(&cy_capsense_context))
                {
                    appStateTimeoutCount = TIMEOUT_RESET;
                }
                else
                {
                    appStateTimeoutCount++;

                    if(ACTIVE_MODE_TIMEOUT < appStateTimeoutCount)
                    {
                        appState = ALR_MODE;
                        appStateTimeoutCount = TIMEOUT_RESET;

                        /* Configure the MSCLP wake up timer as per the ALR mode refresh rate */
                        Cy_CapSense_ConfigureMsclpTimer(ALR_MODE_TIMER, &cy_capsense_context);
                    }
                }

#if ENABLE_RUN_TIME_MEASUREMENT
                activeModeRunTime = StopRuntimeMeasurement();
#endif
                break;
                /* End of ACTIVE_MODE */

            /* Active Low Refresh-rate Mode */
            case ALR_MODE :

                Cy_CapSense_ScanAllSlots(&cy_capsense_context);
                
                interruptStatus = Cy_SysLib_EnterCriticalSection();

                while (Cy_CapSense_IsBusy(&cy_capsense_context))
                {
                    Cy_SysPm_CpuEnterDeepSleep();

                    Cy_SysLib_ExitCriticalSection(interruptStatus);
                    interruptStatus = Cy_SysLib_EnterCriticalSection();
                }
                Cy_SysLib_ExitCriticalSection(interruptStatus);

#if ENABLE_RUN_TIME_MEASUREMENT
                alrModeRunTime = 0u;
                StartRuntimeMeasurement();
#endif

                Cy_CapSense_ProcessAllWidgets(&cy_capsense_context);

                /* Scan, process and check the status of the all Active mode sensors */
                if(Cy_CapSense_IsAnyWidgetActive(&cy_capsense_context))
                {
                    appState = ACTIVE_MODE;
                    appStateTimeoutCount = TIMEOUT_RESET;

                    /* Configure the MSCLP wake up timer as per the ACTIVE mode refresh rate */
                    Cy_CapSense_ConfigureMsclpTimer(ACTIVE_MODE_TIMER, &cy_capsense_context);
                }
                else
                {
                    appStateTimeoutCount++;

                    if(ALR_MODE_TIMEOUT < appStateTimeoutCount)
                    {
                        appState = WOT_MODE;
                        appStateTimeoutCount = TIMEOUT_RESET;
                    }
                }

#if ENABLE_RUN_TIME_MEASUREMENT
                alrModeRunTime = StopRuntimeMeasurement();
#endif
                break;
                /* End of Active-Low Refresh Rate(ALR) mode */

            /* Wake On Touch Mode */
            case WOT_MODE :

                /* Trigger the low power widget scan */
                Cy_CapSense_ScanAllLpSlots(&cy_capsense_context);

                while (Cy_CapSense_IsBusy(&cy_capsense_context))
                {
                    /* Enter and stay in Deep Sleep until WOT timeout or a touch is detected. */
                    /* WOT Timeout = WOT scan interval x Num of frames in WOT (in uSec); 
                    * Refer to Wake-On-Touch settings in CAPSENSE Configurator for WOT Timeout*/

                    Cy_SysPm_CpuEnterDeepSleep ();
                }

                /* Process only the Low Power widgets to detect touch */
                Cy_CapSense_ProcessWidget(CY_CAPSENSE_LOWPOWER0_WDGT_ID, &cy_capsense_context);

                if (Cy_CapSense_IsAnyLpWidgetActive(&cy_capsense_context))
                {
                    appState = ACTIVE_MODE;
                    appStateTimeoutCount = TIMEOUT_RESET;

                    /* Configure the MSCLP wake up timer as per the ACTIVE mode refresh rate */
                    Cy_CapSense_ConfigureMsclpTimer(ACTIVE_MODE_TIMER, &cy_capsense_context);
                }

                else
                {
                    appState = ALR_MODE;
                    appStateTimeoutCount = TIMEOUT_RESET;

                    /* Configure the MSCLP wake up timer as per the ALR mode refresh rate */
                    Cy_CapSense_ConfigureMsclpTimer(ALR_MODE_TIMER, &cy_capsense_context);
                }
                break;
                /* End of "WAKE_ON_TOUCH_MODE" */

            default:
                /** Unknown power mode state. Unexpected situation. **/
                CY_ASSERT(CY_ASSERT_FAILED);
                break;
        }

#if ENABLE_SPI_SERIAL_LED
        /* Refresh LEDs to show latest status */
        UpdateLeds();
#endif

#if ENABLE_TUNER
        /* Establishes synchronized communication with the CAPSENSE&trade; Tuner tool */
        Cy_CapSense_RunTuner(&cy_capsense_context);
#endif

    }
}

/*******************************************************************************
* Function Name: InitializeCapsense
********************************************************************************
* Summary:
*  This function initializes the CAPSENSE and configures the CAPSENSE
*  interrupt.
*
*******************************************************************************/
static void InitializeCapsense(void)
{
    cy_capsense_status_t status = CY_CAPSENSE_STATUS_SUCCESS;

    /* CAPSENSE interrupt configuration MSC 0 */
    const cy_stc_sysint_t msc0InterruptConfig =
    {
        .intrSrc = CY_MSCLP0_LP_IRQ,
        .intrPriority = CAPSENSE_MSC0_INTR_PRIORITY,
    };

    /* Capture the MSC HW block and initialize it to the default state. */
    status = Cy_CapSense_Init(&cy_capsense_context);

    if (CY_CAPSENSE_STATUS_SUCCESS == status)
    {
        /* Initialize CAPSENSE interrupt for MSC 0 */
        Cy_SysInt_Init(&msc0InterruptConfig, Capsense_Msc0Isr);
        NVIC_ClearPendingIRQ(msc0InterruptConfig.intrSrc);
        NVIC_EnableIRQ(msc0InterruptConfig.intrSrc);

        /* Initialize the CAPSENSE firmware modules. */
        status = Cy_CapSense_Enable(&cy_capsense_context);
    }

    if (status != CY_CAPSENSE_STATUS_SUCCESS)
    {
        /* This status could fail before tuning the sensors correctly.
         * Ensure that this function passes after the CAPSENSE sensors are tuned
         * as per procedure give in the Readme.md file */
    }
}

/*******************************************************************************
* Function Name: Capsense_Msc0Isr
********************************************************************************
* Summary:
*  Wrapper function for handling interrupts from CAPSENSE MSC0 block.
*
*******************************************************************************/
static void Capsense_Msc0Isr(void)
{
    Cy_CapSense_InterruptHandler(CY_MSCLP0_HW, &cy_capsense_context);
}

/*******************************************************************************
* Function Name: InitializeCapsenseTuner
********************************************************************************
* Summary:
* EZI2C module to communicate with the CAPSENSE Tuner tool.
*
*******************************************************************************/
static void InitializeCapsenseTuner(void)
{
    cy_en_scb_ezi2c_status_t status = CY_SCB_EZI2C_SUCCESS;

    /* EZI2C interrupt configuration structure */
    const cy_stc_sysint_t ezi2cIntrConfig =
    {
        .intrSrc = CYBSP_EZI2C_IRQ,
        .intrPriority = EZI2C_INTR_PRIORITY,
    };

    /* Initialize the EzI2C firmware module */
    status = Cy_SCB_EZI2C_Init(CYBSP_EZI2C_HW, &CYBSP_EZI2C_config, &ezi2cContext);

    if(status != CY_SCB_EZI2C_SUCCESS)
    {
        CY_ASSERT(CY_ASSERT_FAILED);
    }

    Cy_SysInt_Init(&ezi2cIntrConfig, Ezi2cIsr);
    NVIC_EnableIRQ(ezi2cIntrConfig.intrSrc);

    /* Set the CAPSENSE data structure as the I2C buffer to be exposed to the
     * master on primary slave address interface. Any I2C host tools such as
     * the Tuner or the Bridge Control Panel can read this buffer but you can
     * connect only one tool at a time.
     */
    Cy_SCB_EZI2C_SetBuffer1(CYBSP_EZI2C_HW, (uint8_t *)&cy_capsense_tuner,
                            sizeof(cy_capsense_tuner), sizeof(cy_capsense_tuner),
                            &ezi2cContext);

    Cy_SCB_EZI2C_Enable(CYBSP_EZI2C_HW);
}

/*******************************************************************************
* Function Name: Ezi2cIsr
********************************************************************************
* Summary:
* Wrapper function for handling interrupts from EZI2C block.
*
*******************************************************************************/
static void Ezi2cIsr(void)
{
    Cy_SCB_EZI2C_Interrupt(CYBSP_EZI2C_HW, &ezi2cContext);
}

/*******************************************************************************
* Function Name: RegisterCallback
********************************************************************************
*
* Summary:
*  Register Deep Sleep callbacks for EzI2C, SPI components
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
void RegisterCallback(void)
{
    /* Register EzI2C Deep Sleep callback */
    Cy_SysPm_RegisterCallback(&ezi2cCallback);

#if ENABLE_SPI_SERIAL_LED
    /* Register SPI Deep Sleep callback */
    Cy_SysPm_RegisterCallback(&spiCallback);
#endif

    /* Register Deep Sleep callback */
    Cy_SysPm_RegisterCallback(&deepSleepCb);
}

/*******************************************************************************
* Function Name: DeepSleepCallback
********************************************************************************
*
* Summary:
* Deep Sleep callback implementation. Waits for the completion of SPI transaction.
* And change the SPI GPIOs to highZ while transition to deep-sleep and vice-versa
*
* Parameters:
*  callbackParams: The pointer to the callback parameters structure cy_stc_syspm_callback_params_t.
*  mode: Callback mode, see cy_en_syspm_callback_mode_t
*
* Return:
*  Entered status, see cy_en_syspm_status_t.
*
*******************************************************************************/
cy_en_syspm_status_t DeepSleepCallback(
        cy_stc_syspm_callback_params_t *callbackParams, cy_en_syspm_callback_mode_t mode)
{
    cy_en_syspm_status_t retValue = CY_SYSPM_FAIL;

    switch (mode)
    {
        case CY_SYSPM_CHECK_READY:

            retValue = CY_SYSPM_SUCCESS;
            break;

        case CY_SYSPM_CHECK_FAIL:

            retValue = CY_SYSPM_SUCCESS;
            break;

        case CY_SYSPM_BEFORE_TRANSITION:

            #if ENABLE_SPI_SERIAL_LED
            /* SPI pins drive mode to Analog HighZ */
            Cy_GPIO_SetDrivemode(CYBSP_SPI_MOSI_PORT, CYBSP_SPI_MOSI_PIN, CY_GPIO_DM_ANALOG);
            #endif

            retValue = CY_SYSPM_SUCCESS;
            break;

        case CY_SYSPM_AFTER_TRANSITION:

            #if ENABLE_SPI_SERIAL_LED
            /* SPI pins drive mode to Strong */
            Cy_GPIO_SetDrivemode(CYBSP_SPI_MOSI_PORT, CYBSP_SPI_MOSI_PIN, CY_GPIO_DM_STRONG_IN_OFF);
            #endif

            retValue = CY_SYSPM_SUCCESS;
            break;

        default:
            /* Don't do anything in the other modes */
            retValue = CY_SYSPM_SUCCESS;
            break;
    }
    return retValue;
}

#if ENABLE_RUN_TIME_MEASUREMENT
/*******************************************************************************
* Function Name: StartRuntimeMeasurement
********************************************************************************
* Summary:
*  Initializes the system tick counter by calling Cy_SysTick_Clear() API.
*******************************************************************************/
static void StartRuntimeMeasurement()
{
    Cy_SysTick_Clear();
}

/*******************************************************************************
* Function Name: StopRuntimeMeasurement
********************************************************************************
* Summary:
*  Reads the system tick and converts to time in microseconds(us).
*
*  Returns:
*  runTime - in microseconds(us)
*******************************************************************************/

static uint32_t StopRuntimeMeasurement()
{
    uint32_t ticks;
    uint32_t runTime;
    ticks = Cy_SysTick_GetValue();
    ticks = (SYS_TICK_MAX_INTERVAL - Cy_SysTick_GetValue());
    runTime = (ticks * TIME_PER_TICK_IN_US);
    return runTime;
}
#endif

#if ENABLE_SPI_SERIAL_LED
/*******************************************************************************
* Function Name: UpdateLeds
********************************************************************************
* Summary:
*  Control LEDs in the kit to show the proximity, touch and liquid active status:
*   No Proximity/Touch : LED1 & LED2 = OFF
*   Proximity          : LED1 = GREEN (with brightness proportional to target 
*                        object distance)
*   Touch              : LED1 = BLUE
*******************************************************************************/
void UpdateLeds(void)
{
    /* Brightness of each LED is represented by 0 to 255,
    * where 0 indicates LED in OFF state and 255 indicate maximum
    * brightness of an LED
    */
    uint8_t proxLedBrightness = 0u;
    uint16_t proxMaxRawCount = 0u;
    uint16_t maxDiffCount = 0u;

    uint32_t proxSensorStatus = Cy_CapSense_IsProximitySensorActive(CY_CAPSENSE_PROXIMITY0_WDGT_ID, CY_CAPSENSE_PROXIMITY0_SNS0_ID, &cy_capsense_context);

    // /* Initialize LED values */
    ledContext.serialLedData[LED1].green = 0u;
    ledContext.serialLedData[LED1].blue = 0u;

    /* LED1 and LED2 Control: Check the status of Active mode sensors (proximity sensor) and control LED1 and LED2 accordingly */

        if(proxSensorStatus == PROX_STATE)
        {
            /* Calculate proximity status LED brightness based on target object/hand distance from sensor */
            proxMaxRawCount = cy_capsense_tuner.widgetContext[CY_CAPSENSE_PROXIMITY0_WDGT_ID].maxRawCount;
            maxDiffCount = proxMaxRawCount - cy_capsense_tuner.sensorContext[CY_CAPSENSE_PROXIMITY0_SNS0_ID].bsln;
            proxLedBrightness = (uint8_t)((uint32_t)(cy_capsense_tuner.sensorContext[CY_CAPSENSE_PROXIMITY0_SNS0_ID].diff*255)/maxDiffCount);

            /* LED1 (GREEN) Turns on when proximity is detected */
            ledContext.serialLedData[LED1].green = proxLedBrightness;
        }
        else if(proxSensorStatus >= TOUCH_STATE)
        {
            /* LED1 (GREEN) Turns on when proximity is detected */
            ledContext.serialLedData[LED1].green = 0;

            /* LED2 (BLUE) Turns on when touch is detected */
            ledContext.serialLedData[LED1].blue = SERIAL_LED_BRIGHTNESS_MAX;
        }

    ProcessSerialLed(&ledContext);
}
#endif

/* [] END OF FILE */
