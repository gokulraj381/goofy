/*
  Waveshare ESP32-C6-Touch-LCD-1.47 - pins and LCD bring-up shared by the tests.

  Pins: Waveshare's GPIO table, cross-checked against the official demo package
  (Arduino/examples/01_gfx_helloworld and 04_lvgl_arduino_v8).
*/
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ---- LCD (JD9853, 4-wire SPI) ----
#define LCD_SCK   1   // shared with TF card
#define LCD_MOSI  2   // shared with TF card
#define LCD_CS    14
#define LCD_DC    15
#define LCD_RST   22
#define LCD_BL    23
#define TF_CS     4

// ---- Touch (AXS5106L, I2C addr 0x63) ----
#define TOUCH_SDA 18
#define TOUCH_SCL 19
#define TOUCH_RST 20
#define TOUCH_INT 21

#define LCD_WIDTH   172
#define LCD_HEIGHT  320

// ================= settings to tune from what the screen shows =============
// All defaults below are what Waveshare's 01_gfx_helloworld example does.

// Backlight level that turns it on. Waveshare: digitalWrite(23, HIGH).
#define BL_ON            HIGH

// 1 = send Waveshare's JD9853 register init after gfx->begin() (as their
//     example does). 0 = generic ST7789 init only.
#define USE_JD9853_INIT  1

// Display inversion. Waveshare's JD9853 init ends with command 0x21
// (inversion ON). If white shows as black, flip this.
#define LCD_INVERT       true

// Colour order. Waveshare's init writes MADCTL (0x36) = 0x00 = RGB.
// If red and blue are swapped, set this to true.
#define LCD_BGR          false

// Where the 172-wide panel sits in the controller's 240 columns.
// Waveshare: 34 / 0. Their init also sets CASET 0x22..0xCD = 34..205.
#define LCD_COL_OFFSET   34
#define LCD_ROW_OFFSET   0

#define LCD_ROTATION     0
// ===========================================================================

static Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);

static Arduino_GFX *gfx = new Arduino_ST7789(
  bus, LCD_RST, 0 /* rotation */, false /* IPS */,
  LCD_WIDTH, LCD_HEIGHT,
  LCD_COL_OFFSET, LCD_ROW_OFFSET,
  LCD_COL_OFFSET, LCD_ROW_OFFSET);

// JD9853 init, copied byte-for-byte from Waveshare's demo package:
// ESP32-C6-Touch-LCD-1.47-Demo/Arduino/examples/01_gfx_helloworld
static void lcd_reg_init(void) {
  static const uint8_t init_operations[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,  // sleep out
    END_WRITE,
    DELAY, 120,

    BEGIN_WRITE,
    WRITE_C8_D16, 0xDF, 0x98, 0x53,
    WRITE_C8_D8, 0xB2, 0x23,

    WRITE_COMMAND_8, 0xB7,
    WRITE_BYTES, 4,
    0x00, 0x47, 0x00, 0x6F,

    WRITE_COMMAND_8, 0xBB,
    WRITE_BYTES, 6,
    0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0,

    WRITE_C8_D16, 0xC0, 0x44, 0xA4,
    WRITE_C8_D8, 0xC1, 0x16,

    WRITE_COMMAND_8, 0xC3,
    WRITE_BYTES, 8,
    0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77,

    WRITE_COMMAND_8, 0xC4,
    WRITE_BYTES, 12,
    0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82,

    WRITE_COMMAND_8, 0xC8,
    WRITE_BYTES, 32,
    0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
    0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,

    WRITE_COMMAND_8, 0xD0,
    WRITE_BYTES, 5,
    0x04, 0x06, 0x6B, 0x0F, 0x00,

    WRITE_C8_D16, 0xD7, 0x00, 0x30,
    WRITE_C8_D8, 0xE6, 0x14,
    WRITE_C8_D8, 0xDE, 0x01,

    WRITE_COMMAND_8, 0xB7,
    WRITE_BYTES, 5,
    0x03, 0x13, 0xEF, 0x35, 0x35,

    WRITE_COMMAND_8, 0xC1,
    WRITE_BYTES, 3,
    0x14, 0x15, 0xC0,

    WRITE_C8_D16, 0xC2, 0x06, 0x3A,
    WRITE_C8_D16, 0xC4, 0x72, 0x12,
    WRITE_C8_D8, 0xBE, 0x00,
    WRITE_C8_D8, 0xDE, 0x02,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3,
    0x00, 0x02, 0x00,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3,
    0x01, 0x02, 0x00,

    WRITE_C8_D8, 0xDE, 0x00,
    WRITE_C8_D8, 0x35, 0x00,
    WRITE_C8_D8, 0x3A, 0x05,

    WRITE_COMMAND_8, 0x2A,
    WRITE_BYTES, 4,
    0x00, 0x22, 0x00, 0xCD,

    WRITE_COMMAND_8, 0x2B,
    WRITE_BYTES, 4,
    0x00, 0x00, 0x01, 0x3F,

    WRITE_C8_D8, 0xDE, 0x02,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3,
    0x00, 0x02, 0x00,

    WRITE_C8_D8, 0xDE, 0x00,
    WRITE_C8_D8, 0x36, 0x00,
    WRITE_COMMAND_8, 0x21,
    END_WRITE,

    DELAY, 10,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,  // display on
    END_WRITE
  };
  bus->batchOperation(init_operations, sizeof(init_operations));
}

// Brings the LCD up. Returns false if gfx->begin() failed.
static bool board_lcd_begin(void) {
  // Keep the TF card deselected: it shares SCK and MOSI with the LCD.
  pinMode(TF_CS, OUTPUT);
  digitalWrite(TF_CS, HIGH);

  if (!gfx->begin()) {
    return false;
  }

#if USE_JD9853_INIT
  lcd_reg_init();
#endif

  gfx->setRotation(LCD_ROTATION);
  gfx->invertDisplay(LCD_INVERT);

#if LCD_BGR
  // MADCTL with the BGR bit set (rotation 0 only).
  bus->beginWrite();
  bus->writeC8D8(0x36, 0x08);
  bus->endWrite();
#endif

  gfx->fillScreen(RGB565_BLACK);

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, BL_ON);
  return true;
}
