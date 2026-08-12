/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <stdbool.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
#define printf(...)    printf(__VA_ARGS__)
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;

IWDG_HandleTypeDef hiwdg;

RTC_HandleTypeDef hrtc;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2C1_Init(void);
static void MX_IWDG_Init(void);
static void MX_RTC_Init(void);
/* USER CODE BEGIN PFP */
void 		SleepTight(void);
void 		SensorBringUP(void);
void 		watchdog_reset(void);
uint8_t 	GetSensorRegValue(uint8_t ui8Addr);
void 		GetSensorRegValueSet(uint8_t ui8Addr,  uint8_t *ui8Ptr, uint8_t ui8Size);
void 		SetSensorRegValue(uint8_t ui8Addr, uint8_t ui8Data);
void 		watchdog_set_active(void);
void 		watchdog_set_sleep(void);
uint32_t	GetTemperature(void);
int32_t 	bme280_compensate_T_int32(int32_t i32adc_T, struct bme280_calib_data *BME280_calib);
uint32_t 	bme280_compensate_P_int32(int32_t i32adc_P, struct bme280_calib_data *BME280_calib);
uint32_t 	bme280_compensate_H_int32(int32_t i32adc_H, struct bme280_calib_data *BME280_calib);
void 		bme280_read_calibration(struct bme280_calib_data *BME280_calib);
void 		GetMeasurmentThenUpdate(void);
uint32_t 	ui32_measure_vdd_mv(void);
void 		ConfirmComm(bool bState, uint32_t ui32CommDelay, uint8_t *ui8Data);
void 		WiFiSetup(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Global t_fine carry-over variable required for Pressure & Humidity calculations */
int32_t i32t_BME_fine;

// Returns temperature in DegC, resolution is 0.01 DegC. (e.g. 2500 means 25.00 DegC)
int32_t bme280_compensate_T_int32(int32_t i32adc_T, struct bme280_calib_data *BME280_calib)
{
    int32_t i32var1;
    int32_t i32var2;
    int32_t i32T;

    i32var1 = 0;
    i32var2 = 0;
    i32T = 0;

    i32var1 = ((((i32adc_T >> 3) - ((int32_t)BME280_calib->dig_T1 << 1))) * ((int32_t)BME280_calib->dig_T2)) >> 11;
    i32var2 = (((((i32adc_T >> 4) - ((int32_t)BME280_calib->dig_T1)) * ((i32adc_T >> 4) - ((int32_t)BME280_calib->dig_T1))) >> 12) * ((int32_t)BME280_calib->dig_T3)) >> 14;
    i32t_BME_fine = i32var1 + i32var2;
    i32T = (i32t_BME_fine * 5 + 128) >> 8;
    return i32T;
}

// Returns pressure in Pa as unsigned 32 bit integer
uint32_t bme280_compensate_P_int32(int32_t i32adc_P, struct bme280_calib_data *BME280_calib)
{
    int32_t i32var1;
    int32_t i32var2;
    uint32_t ui32p;

    i32var1 = 0;
    i32var2 = 0;
    ui32p = 0;

    i32var1 = (((int32_t)i32t_BME_fine) >> 1) - (int32_t)64000;
    i32var2 = (((i32var1 >> 2) * (i32var1 >> 2)) >> 11) * ((int32_t)BME280_calib->dig_P6);
    i32var2 = i32var2 + ((i32var1 * ((int32_t)BME280_calib->dig_P5)) << 1);
    i32var2 = (i32var2 >> 2) + (((int32_t)BME280_calib->dig_P4) << 16);
    i32var1 = (((BME280_calib->dig_P3 * (((i32var1 >> 2) * (i32var1 >> 2)) >> 13)) >> 3) + ((((int32_t)BME280_calib->dig_P2) * i32var1) >> 1)) >> 18;
    i32var1 = ((((32768 + i32var1)) * ((int32_t)BME280_calib->dig_P1)) >> 15);

    if (i32var1 != 0)
    {
    	ui32p = (((uint32_t)(((int32_t)1048576) - i32adc_P) - (i32var2 >> 12))) * 3125;
    	if (ui32p < 0x80000000)
    	{
    		ui32p = (ui32p << 1) / ((uint32_t)i32var1);
    	}
    	else
    	{
    		ui32p = (ui32p / (uint32_t)i32var1) * 2;
    	}

    	i32var1 = (((int32_t)BME280_calib->dig_P9) * ((int32_t)(((ui32p >> 3) * (ui32p >> 3)) >> 13))) >> 12;
    	i32var2 = (((int32_t)(ui32p >> 2)) * ((int32_t)BME280_calib->dig_P8)) >> 13;
    	ui32p = (uint32_t)((int32_t)ui32p + ((i32var1 + i32var2 + BME280_calib->dig_P7) >> 4));
    }
    return ui32p;
}

// Returns humidity in %RH as unsigned 32 bit integer, scaled by 1024.
// (e.g. 46080 means 46080 / 1024 = 45.00 %RH)
uint32_t bme280_compensate_H_int32(int32_t i32adc_H, struct bme280_calib_data *BME280_calib)
{
    int32_t i32v_x1_u32r;

    i32v_x1_u32r = 0;

    i32v_x1_u32r = (i32t_BME_fine - ((int32_t)76800));

    i32v_x1_u32r = (((((i32adc_H << 14) - (((int32_t)BME280_calib->dig_H4) << 20) - (((int32_t)BME280_calib->dig_H5) * i32v_x1_u32r)) +
                  ((int32_t)16384)) >> 15) * (((((((i32v_x1_u32r * ((int32_t)BME280_calib->dig_H6)) >> 10) *
                  (((i32v_x1_u32r * ((int32_t)BME280_calib->dig_H3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152)) *
                  ((int32_t)BME280_calib->dig_H2) + ((int32_t)8192)) >> 14));

    i32v_x1_u32r = (i32v_x1_u32r - (((((i32v_x1_u32r >> 15) * (i32v_x1_u32r >> 15)) >> 7) * ((int32_t)BME280_calib->dig_H1)) >> 4));
    i32v_x1_u32r = (i32v_x1_u32r < 0 ? 0 : i32v_x1_u32r);
    i32v_x1_u32r = (i32v_x1_u32r > 419430400 ? 419430400 : i32v_x1_u32r);

    return (uint32_t)(i32v_x1_u32r >> 12);
}

void bme280_read_calibration(struct bme280_calib_data *BME280_calib)
{
    uint8_t ui8buf[26];

    /* Read first block (Dig_T1 to Dig_P9) */
    GetSensorRegValueSet(BME280_REG_DIG_T1, ui8buf, 24);

    BME280_calib->dig_T1 = (uint16_t)((ui8buf[1] << 8) | ui8buf[0]);
    BME280_calib->dig_T2 = (int16_t)((ui8buf[3] << 8) | ui8buf[2]);
    BME280_calib->dig_T3 = (int16_t)((ui8buf[5] << 8) | ui8buf[4]);
    BME280_calib->dig_P1 = (uint16_t)((ui8buf[7] << 8) | ui8buf[6]);
    BME280_calib->dig_P2 = (int16_t)((ui8buf[9] << 8) | ui8buf[8]);
    BME280_calib->dig_P3 = (int16_t)((ui8buf[11] << 8) | ui8buf[10]);
    BME280_calib->dig_P4 = (int16_t)((ui8buf[13] << 8) | ui8buf[12]);
    BME280_calib->dig_P5 = (int16_t)((ui8buf[15] << 8) | ui8buf[14]);
    BME280_calib->dig_P6 = (int16_t)((ui8buf[17] << 8) | ui8buf[16]);
    BME280_calib->dig_P7 = (int16_t)((ui8buf[19] << 8) | ui8buf[18]);
    BME280_calib->dig_P8 = (int16_t)((ui8buf[21] << 8) | ui8buf[20]);
    BME280_calib->dig_P9 = (int16_t)((ui8buf[23] << 8) | ui8buf[22]);

    /* Read single byte H1 */
    BME280_calib->dig_H1 = GetSensorRegValue(BME280_REG_DIG_H1);

    /* Read trailing block for remaining humidity trimmers */
    GetSensorRegValueSet(BME280_REG_DIG_H2, ui8buf, 7);

    BME280_calib->dig_H2 = (int16_t)((ui8buf[1] << 8) | ui8buf[0]);
    BME280_calib->dig_H3 = (uint8_t)ui8buf[2];
    BME280_calib->dig_H4 = (int16_t)((ui8buf[3] << 4) | (ui8buf[4] & 0x0F));
    BME280_calib->dig_H5 = (int16_t)((ui8buf[5] << 4) | (ui8buf[4] >> 4));
    BME280_calib->dig_H6 = (int8_t)ui8buf[6];

}

/* redirect printf output to usart2 (st-link vcp) */
int _write(int file, char *ptr, int len)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)ptr, len, HAL_MAX_DELAY);
    return len;
}

/* set watchdog to short active timeout */
void watchdog_set_active(void)
{
    hiwdg.Init.Prescaler = IWDG_PRESCALER_ACTIVE;
    hiwdg.Init.Reload    = IWDG_RELOAD_ACTIVE;
    HAL_IWDG_Init(&hiwdg);
}

/* set watchdog to maximum sleep timeout */
void watchdog_set_sleep(void)
{
    hiwdg.Init.Prescaler = IWDG_PRESCALER_SLEEP;
    hiwdg.Init.Reload    = IWDG_RELOAD_SLEEP;
    HAL_IWDG_Init(&hiwdg);
}

/* reset watchdog */
void watchdog_reset(void)
{
	/* refresh watchdog to prevent system reset */
	if (HAL_IWDG_Refresh(&hiwdg) != HAL_OK)
	{
	    Error_Handler();
	}
}

/* obtain the register value from BME280 */
uint8_t GetSensorRegValue(uint8_t ui8Addr)
{
	uint8_t ui8Value;
	HAL_I2C_Mem_Read(&hi2c1, BME280_SLAVE<<1, (uint16_t)ui8Addr, 1, &ui8Value, 1, HAL_MAX_DELAY);
	return ui8Value;
}

/* obtain a set of the values from BME280 */
void GetSensorRegValueSet(uint8_t ui8Addr,  uint8_t *ui8Ptr, uint8_t ui8Size)
{
	HAL_I2C_Mem_Read(&hi2c1, BME280_SLAVE<<1, (uint16_t)ui8Addr, 1, ui8Ptr, ui8Size, HAL_MAX_DELAY);
}


/* set the register value of BME280 */
void SetSensorRegValue(uint8_t ui8Addr, uint8_t ui8Data)
{
	HAL_I2C_Mem_Write(&hi2c1, BME280_SLAVE<<1, (uint16_t)ui8Addr, 1, &ui8Data, 1, HAL_MAX_DELAY);
}

/* bring up the BME280 sensor */
void SensorBringUP(void)
{
	/* variable declarations */
	uint8_t ui8Data;
	bool bSuccess;
	uint8_t ui8Retry;

	/* variable initializations */
	ui8Retry = 0;
	ui8Data = 0;
	bSuccess = false;

	/* Attempt to initialize the sensor, if failed, then repeat with RETRY_INIT_CNT retries */
	while ((ui8Retry < RETRY_INIT_CNT) && (bSuccess != true))
	{
		/* Every loop we need to reset a watchdog timer */
		watchdog_reset();
		/* Turn on the power to the sensor */
		HAL_GPIO_WritePin(GPIOB, SENSOR_PWR_Pin, GPIO_PIN_SET);
		/* Wait for 200mS for sensor to stabilize */
		HAL_Delay(INIT_DELAY);
		/* Read ChipID */
		ui8Data = GetSensorRegValue(BME280_REG_ID);
		/* Compare the chipID */
		if (ui8Data != BME280_ID_VAL)
		{
			/* It is bad, comm error */
			printf ("[0]ChipID is %x which bad, attempt #%ld\r\n", (unsigned int)ui8Data, (uint32_t)ui8Retry);
			/* Retry counter increment */
			ui8Retry++;
			/* Turn off the power to the sensor */
			HAL_GPIO_WritePin(GPIOB, SENSOR_PWR_Pin, GPIO_PIN_RESET);
			/* Wait for 500mS for capacitors to discharge */
			HAL_Delay(LONG_RESET_BME280);
		}
		else
		{
			/* Good, we've got comm working */
			printf ("[1]ChipID: %lx\r\n", (uint32_t)ui8Data);
			/* Success flag is set */
			bSuccess = true;
		}
	}
	/* If failure */
	if (!bSuccess)
	{
		/* Print the message then hung up */
		printf ("[2]General env. sensor's failure, reboot by a watchdog timer..\r\n");
		/* ensure i2c lines are idle (released) */
		HAL_I2C_DeInit(&hi2c1);
		/* Power down the sensor */
		HAL_GPIO_WritePin(GPIOB, SENSOR_PWR_Pin, GPIO_PIN_RESET);
		/* This have to trigger the watchdog */
		while (true);
	}
	else
	{
		/* Otherwise reset the watchdog */
		watchdog_reset();
	    /* Apply soft-reset to default values */
		SetSensorRegValue(BME280_REG_RESET, BME280_REG_VALUE_RST);
		HAL_Delay(RESET_DELAY);
		/* Humidity oversampling x1 */
		SetSensorRegValue(BME280_REG_CTRL_HUM, 0x01);
		/* Filter off, standard standby duration */
		SetSensorRegValue(BME280_REG_CONFIG, 0x00);
		/* Temp x1, Press x1, Normal Mode active */
		SetSensorRegValue(BME280_REG_CTRL_MEAS, CTRL_BME280);
		HAL_Delay(RESET_DELAY);
		watchdog_reset();
	}
}

void SleepTight(void)
{
	/* enable WKUP5 (PB5) wakeup before standby */
	HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1);
	/* clear wakeup flag */
	__HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);
	/* Sleep tight */
	HAL_PWR_EnterSTANDBYMode();
}

/* WiFi setup request */
void WiFiSetup(void)
{
	/* Global variables */
	uint8_t ui8pBuf[MID_BUFFER_SIZE];

	printf ("[9]Woken up for setup\r\n");
	HAL_GPIO_WritePin(GPIOB, Power_WiFi_BT_Pin, GPIO_PIN_SET);
	/* Empty string */
	sprintf ((char*)ui8pBuf, "%c", '\0');
    /* watchdog reset */
 	watchdog_reset();
	/* Confirm that part is awoke */
 	ConfirmComm(GPIO_PIN_RESET, ESP_GENTIMEOUT, ui8pBuf);
	/* Request setup */
 	sprintf ((char*)ui8pBuf, "Setup requested\r\n");
    /* watchdog reset */
 	watchdog_reset();
	/* Wait for confiramtion */
 	ConfirmComm(GPIO_PIN_SET, ESP_GENTIMEOUT, ui8pBuf);
    /* watchdog reset */
 	watchdog_reset();
 	printf ("[11]Setting up..\r\n");
 	/* Empty string */
	sprintf ((char*)ui8pBuf, "%c", '\0');
	/* Watchdog reset */
	watchdog_reset();
	/* Setup timeout is calculated as following: 1000mS*60*10 = 600000 or 10 minutes */
	ConfirmComm(GPIO_PIN_RESET, SETUP_TIMEOUT, ui8pBuf);
	printf("[12] Confirmed, sleep now.. \r\n");

	/* Go to sleep */
	SleepTight();
}

/* Check the reset reason, do we need to wakeup fully or not */
void WakeReasonChk(void)
{
	/* Declare variables */
	uint32_t ui32SleepValue;

	/* Initialize variables */
	ui32SleepValue = 0;

	/* If reason is a setup button then we need to proceed to WiFi setup */
	if (HAL_GPIO_ReadPin(GPIOA, SetupButton_Pin))
	{
		WiFiSetup();
	}
	/* Get the counter out the variables which are not killed by deep sleep */
	ui32SleepValue = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0);
	printf ("[3]Woken up %ld times, counter increased to %ld\r\n", ui32SleepValue, TIMEOUT_MASK&(ui32SleepValue + 1));
	/* if value not 0 then increase it by 1 and jump to sleep afterwards */
	if (ui32SleepValue)
	{
		/* Increase by 1 gated by TIMEOUT_MASK just in case if stored value appears too large */
		HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, TIMEOUT_MASK&(ui32SleepValue + 1));
		watchdog_set_sleep();
		/* Go to sleep */
		SleepTight();
	}
}

/* measure vdd in millivolts using internal vrefint */
/* returns 0 on failure */
uint32_t ui32_measure_vdd_mv(void)
{
    /* Declare variables */
	uint32_t ui32_adc_raw;
    uint32_t ui32_vdd_mv;
    uint32_t ui32_tick_start;
    bool b_success;

    /* Initialize variables */
    ui32_adc_raw = 0u;
    ui32_vdd_mv = ADC_INVALID_RESULT;
    ui32_tick_start = 0u;
    b_success = true;

    /* enable adc clock */
    __HAL_RCC_ADC1_CLK_ENABLE();

    /* set adc clock mode to PCLK/2 */
    ADC1->CFGR2 = ADC_CFGR2_CKMODE_0;

    /* run calibration - must be done with ADEN=0 */
    ADC1->CR |= ADC_CR_ADCAL;

    /* Get the start time */
    ui32_tick_start = HAL_GetTick();

    /* Calibrate the ADC readout */
    while (((ADC1->CR & ADC_CR_ADCAL) != 0u) && (b_success == true))
    {
        /* reset watchdog */
    	watchdog_reset();

    	/* Check if calibration timeout expired */
    	if ((HAL_GetTick() - ui32_tick_start) >= ADC_TIMEOUT_MS)
        {
    		printf("[4]Warning: core voltage measurement failed - calibration timeout\r\n");
            /* Set the flag */
            b_success = false;
        }
    }

    /* If there was no error reported */
    if (b_success == true)
    {
        /* enable vrefint */
        ADC1_COMMON->CCR |= ADC_CCR_VREFEN;

        /* clear adc ready flag if set */
        if ((ADC1->ISR & ADC_ISR_ADRDY) != 0u)
        {
            ADC1->ISR |= ADC_ISR_ADRDY;
        }

        /* enable adc */
        ADC1->CR |= ADC_CR_ADEN;

        /* Get the start time */
        ui32_tick_start = HAL_GetTick();
        while (((ADC1->ISR & ADC_ISR_ADRDY) == 0u) && (b_success == true))
        {
           /* watchdog reset */
        	watchdog_reset();
        	/* Check if measurment timeout expired */
            if ((HAL_GetTick() - ui32_tick_start) >= ADC_TIMEOUT_MS)
            {
            	printf("[5]Warning: core voltage measurement failed - ADC ready timeout\r\n");
                /* Set the flag */
                b_success = false;
            }
        }
    }

    /* If success */
    if (b_success == true)
    {
        /* select vrefint channel 17 */
        ADC1->CHSELR = ADC_CHSELR_CHSEL17;

        /* set sample time - longest for vrefint (239.5 cycles) */
        ADC1->SMPR = ADC_SMPR_SMP_0 | ADC_SMPR_SMP_1 | ADC_SMPR_SMP_2;

        /* wait for vrefint to stabilise */
        HAL_Delay(1u);

        /* start conversion */
        ADC1->CR |= ADC_CR_ADSTART;

        ui32_tick_start = HAL_GetTick();
        while (((ADC1->ISR & ADC_ISR_EOC) == 0u) && (b_success == true))
        {
        	/* watchdog reset */
        	watchdog_reset();
            /* Check if measurment timeout expired */
            if ((HAL_GetTick() - ui32_tick_start) >= ADC_TIMEOUT_MS)
            {
            	printf("[6]Warning: core voltage measurement failed - conversion timeout\r\n");
                /* Set the flag */
                b_success = false;
            }
        }
    }

    /* If entire operation is successfull */
    if (b_success == true)
    {
        ui32_adc_raw = ADC1->DR;
        /* Read the voltage */
        ui32_vdd_mv = (uint32_t)(VREFINT_CAL_VREF_MV *
                      (uint32_t)(*VREFINT_CAL_ADDR) /
                      ui32_adc_raw);
    }

    /* cleanup always runs regardless of success or failure */
    ADC1->CR |= ADC_CR_ADDIS;
    ADC1_COMMON->CCR &= ~ADC_CCR_VREFEN;
    __HAL_RCC_ADC1_CLK_DISABLE();

    return ui32_vdd_mv;
}

void ConfirmComm(bool bState, uint32_t ui32CommDelay, uint8_t *ui8Data)
{
	/* Declare variables */
	uint32_t ui32EspTimeout;
	bool bClean;

	/* Initialize variables */
	ui32EspTimeout = 0;
	bClean = false;

	/* Reset the watchdog */
	watchdog_reset();
	/* Wait until XIAO confirms that it is reset */
	ui32EspTimeout = HAL_GetTick();
	/* But no longer than for an ESP_GENTIMEOUT */
	while (((HAL_GetTick() - ui32EspTimeout) < ui32CommDelay) && (bClean == false))
	{
		/* Reset watchdog during wait */
		watchdog_reset();
		/* Transmit the weather data in loop until ESP32 gets it */
		printf ("%s", ui8Data);
		/* Set the flag if ESP32 is ok _OR_ if button is pressed during the process */
		if (HAL_GPIO_ReadPin(GPIOA, ESP_FEEDBACK_Pin) == bState || (HAL_GetTick() > HAL_BUTTON_TIMEOUT && HAL_GPIO_ReadPin(GPIOA, SetupButton_Pin)))
		{
			bClean = true;
		}
	}

	/* Failure processing */
	if (!bClean)
	{
		printf ("[7]Failure of ESP32 XIAO, it did not send a value of %ld\r\n", (uint32_t)bState);
		/* Toggle the power to ESP32 off */
		HAL_GPIO_WritePin(GPIOB, SENSOR_PWR_Pin, GPIO_PIN_RESET);
		/* WDT shall activate here */
		while (true)
		{
			/* Trigger watchdog */
		}
	}
	printf ("[8]Got XIAO comm confirmation with status = %ld and timeout = %ld\r\n", (uint32_t)bState, ui32CommDelay);
}

void GetMeasurmentThenUpdate(void)
{
	/* Variables according to BME280 datasheet */
	struct bme280_calib_data BME280_calib_data_rb;
	uint8_t ui8buf[8];

	/* Declare variables */
	int32_t i32adc_P;
	int32_t i32adc_T;
	int32_t i32adc_H;
	uint8_t ui8pBuf[MID_BUFFER_SIZE];

	/* Assign initial values to them */
	i32adc_P = 0;
	i32adc_T = 0;
	i32adc_H = 0;

	/* Wakeup wifi and bt backend */
	HAL_GPIO_WritePin(GPIOB, Power_WiFi_BT_Pin, GPIO_PIN_SET);
	/* Reset watchdog */
	watchdog_reset();
	HAL_Delay(INIT_DELAY);watchdog_reset();

	watchdog_reset();
	/* Confirm that ESP32 is started **/
	sprintf ((char*)ui8pBuf, "%c", '\0');
	ConfirmComm(GPIO_PIN_RESET, ESP_GENTIMEOUT, ui8pBuf);
	/* Obtain the measurements */
	GetSensorRegValueSet(BME280_REG_DATA_START, ui8buf, 8);

    /*
     * Get the data into the buffer in the following format:
     * Temperature in integer, expressed in C is presented multiplied by 100 to preserve the fraction digits, i.e. 21.45C will be represented as 2145
     * Pressure will be shown in Pa, for example 99329 is expressed in Pa
     * Humidity will be shown as value * 1000, for example 70352 will mean 70.352%
     */
	i32adc_P = (int32_t)(((uint32_t)ui8buf[0] << 12) | ((uint32_t)ui8buf[1] << 4) | ((uint32_t)ui8buf[2] >> 4));
    i32adc_T = (int32_t)(((uint32_t)ui8buf[3] << 12) | ((uint32_t)ui8buf[4] << 4) | ((uint32_t)ui8buf[5] >> 4));
    i32adc_H = (int32_t)(((uint32_t)ui8buf[6] << 8) | (uint32_t)ui8buf[7]);

    /* Now we need to apply correction at calibration */
    bme280_read_calibration(&BME280_calib_data_rb);

    /* Print all including to a wifi                                                  */
    /* This is the only string which does not have [..] as it will be parsed by ESP32 */
    /* This is done via the string just for the case that it might take some time to  */
    /* boot ESP32 and STM32 will have to repeat the transmission until ESP32 confirms */
    /* that it got it otherwise STM32 will initiate the reset via the watchdog        */
	sprintf ((char*)ui8pBuf, "Temperature: %ld Pressure: %ld Humidity: %ld Battery: %ld\r\n",
			bme280_compensate_T_int32(i32adc_T, &BME280_calib_data_rb),
			bme280_compensate_P_int32(i32adc_P, &BME280_calib_data_rb),
			bme280_compensate_H_int32(i32adc_H, &BME280_calib_data_rb),
			ui32_measure_vdd_mv());
	/* Reset the watchdog */
	watchdog_reset();
	/* Confirm that ESP32 is started **/
	ConfirmComm(GPIO_PIN_SET, WIFI_NORMAL, ui8pBuf);

	HAL_Delay(INIT_DELAY);
	/* Reset the watchdog */
	watchdog_reset();
	/* Confirm that ESP32 is started */
	sprintf ((char*)ui8pBuf, "%c", '\0');
	ConfirmComm(GPIO_PIN_RESET, WIFI_NORMAL, ui8pBuf);
	watchdog_reset();
	HAL_Delay(INIT_DELAY);
	watchdog_reset();
	/* Power down the sensor */
	HAL_GPIO_WritePin(GPIOB, SENSOR_PWR_Pin, GPIO_PIN_RESET);
	/* Reset the watchdog */
	watchdog_reset();
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_IWDG_Init();
  MX_RTC_Init();
  /* USER CODE BEGIN 2 */
  /* We need to initialize UART if we are in debug mode here as we will need all the printfs */
  MX_USART2_UART_Init();
  /* Set the watchdog parameters */
  watchdog_set_active();
  /* Reset the watchdog */
  watchdog_reset();
  /* We need to count how many times we have woken up */
  WakeReasonChk();
  /* Reset the wakeup counter to 0x01 */
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, 1);
  /* Print the message so we could see what is going on */
  printf("[10]General start..\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	  /* Initialize the sensor */
	  SensorBringUP();
	  GetMeasurmentThenUpdate();
	  /* ensure i2c lines are idle (released) */
	  HAL_I2C_DeInit(&hi2c1);
	  /* Power down the sensor */
	  HAL_GPIO_WritePin(GPIOB, SENSOR_PWR_Pin, GPIO_PIN_RESET);
	  /* Power down the Wi-Fi & BT board */
	  HAL_GPIO_WritePin(GPIOB, Power_WiFi_BT_Pin, GPIO_PIN_RESET);
	  watchdog_set_sleep();
	  watchdog_reset();
	  /* Go to sleep */
	  SleepTight();
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_5;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_MSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART2|RCC_PERIPHCLK_I2C1
                              |RCC_PERIPHCLK_RTC;
  PeriphClkInit.Usart2ClockSelection = RCC_USART2CLKSOURCE_PCLK1;
  PeriphClkInit.I2c1ClockSelection = RCC_I2C1CLKSOURCE_PCLK1;
  PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x00000608;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief IWDG Initialization Function
  * @param None
  * @retval None
  */
static void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
  hiwdg.Init.Window = 4095;
  hiwdg.Init.Reload = 4095;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */

  /* USER CODE END RTC_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 9600;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, Power_WiFi_BT_Pin|SENSOR_PWR_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : SetupButton_Pin ESP_FEEDBACK_Pin */
  GPIO_InitStruct.Pin = SetupButton_Pin|ESP_FEEDBACK_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : Power_WiFi_BT_Pin SENSOR_PWR_Pin */
  GPIO_InitStruct.Pin = Power_WiFi_BT_Pin|SENSOR_PWR_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
