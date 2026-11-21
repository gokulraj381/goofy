/*
  Display test for Waveshare ESP32-C6-Touch-LCD-1.47 (172 x 320, JD9853)

  What you should see, repeating:
    1. full-screen red, green, blue, white, black (1 s each), each with its
       name printed in the middle
    2. a white outline exactly on the screen edge with a yellow cross,
       eight colour bars, and text

  Settings to tune are in include/board.h.
*/

#include "board.h"

static void showPattern() {
  const int16_t w = gfx->width();
  const int16_t h = gfx->height();

  gfx->fillScreen(RGB565_BLACK);

  // Outline on the very edge: all four sides must be visible.
  gfx->drawRect(0, 0, w, h, RGB565_WHITE);

  // Cross through the centre.
  gfx->drawLine(0, 0, w - 1, h - 1, RGB565_YELLOW);
  gfx->drawLine(w - 1, 0, 0, h - 1, RGB565_YELLOW);

  // Eight colour bars across the middle.
  const uint16_t bars[8] = {
    RGB565_WHITE, RGB565_YELLOW, RGB565_CYAN, RGB565_GREEN,
    RGB565_MAGENTA, RGB565_RED, RGB565_BLUE, RGB565_BLACK
  };
  const int16_t barW = (w - 12) / 8;
  for (int i = 0; i < 8; i++) {
    gfx->fillRect(6 + i * barW, 110, barW, 60, bars[i]);
  }

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(10, 20);
  gfx->println("ESP32-C6");
  gfx->setCursor(10, 44);
  gfx->println("1.47 Touch");

  gfx->setTextSize(1);
  gfx->setCursor(10, 76);
  gfx->print(w);
  gfx->print(" x ");
  gfx->print(h);
  gfx->println(" px");

  gfx->setTextColor(RGB565_GREEN);
  gfx->setTextSize(2);
  gfx->setCursor(10, 200);
  gfx->println("Display OK");

  gfx->setTextColor(RGB565_RED);
  gfx->setCursor(10, 240);
  gfx->print("R ");
  gfx->setTextColor(RGB565_GREEN);
  gfx->print("G ");
  gfx->setTextColor(RGB565_BLUE);
  gfx->print("B");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Display test starting");

  if (!board_lcd_begin()) {
    Serial.println("gfx->begin() failed");
  } else {
    Serial.println("gfx->begin() ok");
  }
}

void loop() {
  const uint16_t fills[5] = {
    RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_WHITE, RGB565_BLACK
  };
  const char *names[5] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK"};

  for (int i = 0; i < 5; i++) {
    gfx->fillScreen(fills[i]);
    // Label each fill so a wrong colour is obvious. Black on white, white
    // on everything else.
    gfx->setTextColor(i == 3 ? RGB565_BLACK : RGB565_WHITE);
    gfx->setTextSize(3);
    gfx->setCursor(20, 148);
    gfx->print(names[i]);
    Serial.print("fill: ");
    Serial.println(names[i]);
    delay(1000);
  }

  showPattern();
  Serial.println("pattern");
  delay(5000);
}
