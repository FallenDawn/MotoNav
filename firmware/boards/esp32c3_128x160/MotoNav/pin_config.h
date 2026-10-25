#pragma once

// AirM2M Core ESP32-C3 + external ST7735 1.8-inch 128x160 SPI display.
#define LCD_SCK   2
#define LCD_MOSI  3
#define LCD_MISO  -1
#define LCD_CS    7
#define LCD_DC    4
#define LCD_RESET 0
// Backlight is hardwired to 3V3 on this wiring; there is no software pin.
#define LCD_BL    -1
#define LCD_WIDTH 128
#define LCD_HEIGHT 160

#define IIC_SDA -1
#define IIC_SCL -1
#define TP_INT -1
#define TP_RESET -1
#define BOARD_TOUCH_FLIP_X 0
#define BOARD_TOUCH_FLIP_Y 0
#define MOTONAV_SMALL_SCREEN 1

const int SDMMC_CLK = -1;
const int SDMMC_CMD = -1;
const int SDMMC_DATA = -1;
const int SDMMC_CS = -1;
