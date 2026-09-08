#pragma once

// Project-owned TFT_eSPI configuration. This file is force-included for the
// application and every TFT_eSPI translation unit by platformio.ini.
#define USER_SETUP_LOADED
#define USER_SETUP_INFO "HSD-9190J-B7 ST7796 on ESP32-WROVER-E"
#define USER_SETUP_ID 9190

#define ST7796_DRIVER

// HSD-9190J-B7 exposes a normal 4-wire SPI interface. RPI_DISPLAY_TYPE is
// intentionally not defined because it changes ESP32 commands to 16-bit
// Raspberry Pi shield transfers, which this panel does not acknowledge.

// VSPI default bus pins on the original ESP32.
#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18

// Control pins deliberately avoid ESP32 boot-strapping and WROVER PSRAM pins.
#define TFT_CS 27
#define TFT_DC 26
#define TFT_RST 25

#define SPI_FREQUENCY 20000000

// Font 1 is used for connection and authentication status messages.
#define LOAD_GLCD
