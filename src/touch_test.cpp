/*
  Touch test for Waveshare ESP32-C6-Touch-LCD-1.47 (AXS5106L over I2C)

  Draws a dot where you touch and prints the coordinates on screen and on
  Serial. Hold the BOOT button (GPIO8) to clear the screen.

  Uses Waveshare's own driver (lib/esp_lcd_touch_axs5106l, copied unchanged
  from the demo package) the same way their 04_lvgl_arduino_v8 example does:
  Wire.begin(18, 19), then bsp_touch_init(&Wire, RST 20, INT 21, ...).
*/

#include <Wire.h>
#include "board.h"
#include "esp_lcd_touch_axs5106l.h"

#define BOOT_BTN 8

static const uint16_t dotColours[MAX_TOUCH_MAX_POINTS] = {
  RGB565_YELLOW, RGB565_CYAN, RGB565_MAGENTA, RGB565_GREEN, RGB565_RED
};

static void drawFrame() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawRect(0, 0, gfx->width(), gfx->height(), RGB565_WHITE);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(8, 8);
  gfx->print("Touch test - BOOT clears");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Touch test starting");

  pinMode(BOOT_BTN, INPUT_PULLUP);

  if (!board_lcd_begin()) {
    Serial.println("gfx->begin() failed");
  }
  drawFrame();

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  bsp_touch_init(&Wire, TOUCH_RST, TOUCH_INT,
                 gfx->getRotation(), gfx->width(), gfx->height());
  Serial.println("touch init done");
}

void loop() {
  touch_data_t touch;

  bsp_touch_read();
  if (bsp_touch_get_coordinates(&touch)) {
    for (uint8_t i = 0; i < touch.touch_num && i < MAX_TOUCH_MAX_POINTS; i++) {
      const int16_t x = touch.coords[i].x;
      const int16_t y = touch.coords[i].y;
      gfx->fillCircle(x, y, 3, dotColours[i]);
      Serial.printf("touch %u: x=%d y=%d\n", i, x, y);
    }

    // Show the first point's coordinates in a strip under the title.
    gfx->fillRect(1, 20, gfx->width() - 2, 10, RGB565_BLACK);
    gfx->setTextColor(RGB565_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(8, 21);
    gfx->printf("x=%d y=%d n=%u", touch.coords[0].x, touch.coords[0].y,
                touch.touch_num);
  }

  if (digitalRead(BOOT_BTN) == LOW) {
    drawFrame();
    delay(200);
  }

  delay(5);
}
