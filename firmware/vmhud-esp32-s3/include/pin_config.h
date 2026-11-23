#pragma once

// LilyGO T-Display S3 pin definitions
#define PIN_LCD_BL 38
#define PIN_POWER_ON 15
#define PIN_LCD_RES 5
#define PIN_LCD_CS 6
#define PIN_LCD_DC 7

// Default I2C pins for ESP32-S3 Super Mini + external OLED
#ifndef OLED_SDA
#define OLED_SDA 8
#endif

#ifndef OLED_SCL
#define OLED_SCL 9
#endif
