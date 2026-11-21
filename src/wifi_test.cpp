/*
  WiFi radio test for Waveshare ESP32-C6-Touch-LCD-1.47

  Checks both directions of the radio:
    - sending:   the board creates a network called "KeychainTest". If a
                 phone or PC nearby can see it, the board can transmit.
    - receiving: every 10 seconds it scans and lists the networks it can
                 hear, on the display and on Serial.
*/

#include <WiFi.h>
#include "board.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("WiFi test starting");

  // First scan with the display still off, to see whether the screen or its
  // backlight disturbs the radio.
  WiFi.mode(WIFI_STA);
  const int before = WiFi.scanNetworks(false, true, false, 300);
  Serial.print("scan with display off found ");
  Serial.println(before);
  WiFi.scanDelete();

  board_lcd_begin();
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 4);
  gfx->print("WiFi test: KeychainTest");

  WiFi.mode(WIFI_AP_STA);
  const bool ap = WiFi.softAP("KeychainTest", "keychain123", 6);
  Serial.print("own network started: ");
  Serial.println(ap ? "yes" : "NO");
  Serial.print("MAC: ");
  Serial.println(WiFi.softAPmacAddress());
}

void loop() {
  const int n = WiFi.scanNetworks(false, true, false, 300);
  Serial.print("scan found ");
  Serial.println(n);

  gfx->fillRect(0, 20, gfx->width(), gfx->height() - 20, RGB565_BLACK);
  gfx->setCursor(4, 22);
  gfx->setTextColor(n > 0 ? RGB565_GREEN : RGB565_RED);
  gfx->print("networks heard: ");
  gfx->print(n);
  gfx->setTextColor(RGB565_WHITE);

  for (int i = 0; i < n; i++) {
    Serial.print("  '");
    Serial.print(WiFi.SSID(i));
    Serial.print("'  ch ");
    Serial.print(WiFi.channel(i));
    Serial.print("  ");
    Serial.print(WiFi.RSSI(i));
    Serial.println(" dBm");
    if (i < 20) {
      gfx->setCursor(4, 36 + i * 12);
      gfx->print(WiFi.SSID(i).substring(0, 18));
      gfx->print(" ");
      gfx->print(WiFi.RSSI(i));
    }
  }
  WiFi.scanDelete();
  delay(10000);
}
