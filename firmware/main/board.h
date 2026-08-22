/* Pin map: Zhengchen ZC-1.54TFT-WiFi (from xiaozhi boards/zhengchen/1.54tft-wifi) */
#pragma once
#include "driver/gpio.h"

/* I2S microphone (INMP441-class, standard I2S) */
#define MIC_I2S_WS      GPIO_NUM_4
#define MIC_I2S_SCK     GPIO_NUM_5
#define MIC_I2S_DIN     GPIO_NUM_6
#define MIC_SAMPLE_RATE 16000

/* I2S speaker amp (no PA enable pin — confirmed against stock board file) */
#define SPK_I2S_DOUT    GPIO_NUM_7
#define SPK_I2S_BCLK    GPIO_NUM_15
#define SPK_I2S_LRCK    GPIO_NUM_16
#define SPK_SAMPLE_RATE 16000

/* Buttons (active low). Physical layout: separate power button on the case's
 * left edge; three buttons in a row on top, left-to-right = vol-down / analyze
 * / vol-up — confirmed against the physical device 2026-08-22. */
#define BTN_MAIN        GPIO_NUM_0   /* middle top button: analyze (single press) */
#define BTN_UP          GPIO_NUM_10  /* right top button: vol-up / push-to-talk (hold) */
#define BTN_DOWN        GPIO_NUM_39  /* left top button: vol-down, and chord modifier for vol-up */

/* ST7789 240x240 SPI LCD */
#define LCD_SPI_MOSI    GPIO_NUM_41
#define LCD_SPI_SCLK    GPIO_NUM_42
#define LCD_RST         GPIO_NUM_45
#define LCD_DC          GPIO_NUM_40
#define LCD_CS          GPIO_NUM_21
#define LCD_BACKLIGHT   GPIO_NUM_20
#define LCD_H_RES       240
#define LCD_V_RES       240

/* Battery */
#define BAT_ADC_CHANNEL   ADC_CHANNEL_7 /* ADC1 */
#define BAT_CHARGING_GPIO GPIO_NUM_9    /* high while charger attached (stock power_manager) */

/* Recording */
#define RECORD_SECONDS  7
#define TALK_MAX_SECONDS 15  /* push-to-talk hard ceiling; release stops it sooner */
