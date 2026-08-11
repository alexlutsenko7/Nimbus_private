/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define SetupButton_Pin GPIO_PIN_0
#define SetupButton_GPIO_Port GPIOA
#define ESP_FEEDBACK_Pin GPIO_PIN_1
#define ESP_FEEDBACK_GPIO_Port GPIOA
#define VCP_TX_Pin GPIO_PIN_2
#define VCP_TX_GPIO_Port GPIOA
#define TMS_Pin GPIO_PIN_13
#define TMS_GPIO_Port GPIOA
#define TCK_Pin GPIO_PIN_14
#define TCK_GPIO_Port GPIOA
#define VCP_RX_Pin GPIO_PIN_15
#define VCP_RX_GPIO_Port GPIOA
#define Power_WiFi_BT_Pin GPIO_PIN_3
#define Power_WiFi_BT_GPIO_Port GPIOB
#define SENSOR_PWR_Pin GPIO_PIN_4
#define SENSOR_PWR_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* Power supply measurment via ADC constants */
#define MID_BUFFER_SIZE			128

#define ESP_GENTIMEOUT			1000u
#define WIFI_NORMAL				30000u
/* Setup timeout is calculated as following: 1000mS*60*10 = 600000 or 10 minutes */
#define SETUP_TIMEOUT			600000

#define VREFINT_CAL_ADDR    	((uint16_t *)0x1FF80078UL)
#define VREFINT_CAL_VREF_MV    	3000UL
#define ADC_TIMEOUT_MS          1000u
#define ADC_INVALID_RESULT      0u

#define DEBUG_MODE        0

/*
 * ---------------------------------------------------------------------------
 * Wake-up / IWDG timing
 * ---------------------------------------------------------------------------
 * No RTC wakeup timer is configured in this project (see MX_RTC_Init) -
 * PWR_WAKEUP_PIN1 only wakes the part on an external pin edge (the setup
 * button). The periodic wake from STANDBY is therefore driven entirely by
 * the IWDG: it keeps counting down off LSI while the MCU is in STANDBY, and
 * when it reaches 0 it forces a full chip reset - which functions as
 * "waking up" (see watchdog_set_sleep() / SleepTight() in main.c). There is
 * no separate low-power timer involved; the watchdog timeout below IS the
 * wake period, so changing it changes how often the station reports.
 *
 * IWDG timeout formula (STM32L0, 12-bit down counter):
 *
 *     T [s] = (Reload + 1) x PrescalerDivider / LSI_Hz
 *
 * PrescalerDivider is 4 x 2^N for IWDG_PRESCALER_4..256, i.e. one of
 * {4, 8, 16, 32, 64, 128, 256}. LSI on STM32L0 is nominally ~37 kHz but is
 * NOT calibrated by default and can drift roughly +/-30-50% with
 * temperature/voltage - treat every timing below as approximate, not exact.
 *
 * Two independent profiles get loaded into the same IWDG peripheral at
 * different points in the code (watchdog_set_active() / watchdog_set_sleep()
 * in main.c), because they serve different purposes:
 *
 *   _ACTIVE - armed while the MCU is awake doing real work (BME280 I2C
 *             reads, the UART handshake with the ESP32 in ConfirmComm).
 *             Kept SHORT so a genuine hang (e.g. a HAL_I2C_* call blocking
 *             forever on a wedged bus, since those are called with
 *             HAL_MAX_DELAY) is caught and the part resets quickly instead
 *             of sitting dead until the battery runs out.
 *             Reload=4095, Prescaler=32 -> (4096 x 32) / 37000 ~= 3.5s.
 *
 *   _SLEEP  - armed just before entering STANDBY. Kept LONG, and its expiry
 *             *is* the wake-up mechanism described above.
 *             Reload=4095, Prescaler=128 -> (4096 x 128) / 37000 ~= 14.2s.
 *
 * A single _SLEEP expiry is NOT the full reporting interval by itself,
 * though: on every wake, WakeReasonChk() (main.c) checks a counter kept in
 * RTC backup register RTC_BKP_DR0 - this survives STANDBY, unlike ordinary
 * RAM. The counter walks 1, 2, 3, ... up to TIMEOUT_MASK (defined further
 * below), then wraps back to 0; only on the wake where it reads 0 does the
 * device actually power up the sensor and the ESP32 and report a
 * measurement (GetMeasurmentThenUpdate()) - every other wake just re-arms
 * _SLEEP and goes straight back to STANDBY. So the real formula is:
 *
 *     reporting period ~= (TIMEOUT_MASK + 1) x _SLEEP timeout
 *
 * With TIMEOUT_MASK = 0x3F (64) and the _SLEEP numbers above:
 *
 *     64 x 14.2s ~= 906s ~= 15.1 minutes
 *
 * DEBUG_MODE=1 shrinks everything at once (8x prescaler for both profiles,
 * TIMEOUT_MASK=0xF) so a full sense-and-report cycle takes ~14s on the bench
 * instead of ~15 minutes, so you're not waiting to see whether an upload
 * works while iterating on the code.
 * ---------------------------------------------------------------------------
 */
#if DEBUG_MODE == 1
#define IWDG_PRESCALER_ACTIVE   IWDG_PRESCALER_8
#else
#define IWDG_PRESCALER_ACTIVE   IWDG_PRESCALER_32
#endif
#define IWDG_RELOAD_ACTIVE      4095u

#if DEBUG_MODE == 1
#define IWDG_PRESCALER_SLEEP    IWDG_PRESCALER_8
#else
#define IWDG_PRESCALER_SLEEP    IWDG_PRESCALER_128
#endif
#define IWDG_RELOAD_SLEEP       4095u

#define RETRY_INIT_CNT			5
#define INIT_DELAY				200
#define RESET_DELAY				20
#define LONG_RESET_BME280		500

/* Temp x1, Press x1, Forced Mode active */
#define CTRL_BME280				0x25

/* Reg spec */
#define BME280_SLAVE			0x77
#define BME280_ID_VAL			0x60

#define BME280_REG_ID			0xD0

#define BME280_REG_DIG_T1      	0x88
#define BME280_REG_DIG_H1      	0xA1
#define BME280_REG_DIG_H2      	0xE1

#define BME280_REG_RESET       	0xE0
#define BME280_REG_CTRL_HUM    	0xF2
#define BME280_REG_STATUS      	0xF3
#define BME280_REG_CTRL_MEAS   	0xF4
#define BME280_REG_CONFIG      	0xF5
#define BME280_REG_DATA_START  	0xF7

#define BME280_REG_VALUE_RST	0xB6

#define DEFAULT_TIMEOUT			0xFFFF
#if DEBUG_MODE == 1
#define TIMEOUT_MASK			0xF
#else
#define TIMEOUT_MASK			0x3F
#endif

#define HAL_BUTTON_TIMEOUT		500
// Calibration structure containing values stored in the sensor's NVM
struct bme280_calib_data {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;

    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;

    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;
};

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
