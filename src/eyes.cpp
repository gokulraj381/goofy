/*
  Cute eyes for Waveshare ESP32-C6-Touch-LCD-1.47 (landscape, 320 x 172)
  Made for the cat-shaped case: the case's ears are the top of the picture.

  What it shows (eyes only, no face):
    - two big eyes that look left, blink, look right, blink, and repeat
    - touch the screen (pet the cat) and the eyes turn into happy arcs
    - press the BOOT button, or hold a finger on the screen for 1.5 seconds, to
      switch to the next look (see LOOKS below); the choice is remembered

  Everything is drawn into an off-screen buffer with smoothed edges and then
  pushed to the LCD in one go, so there is no flicker.

  Things to tune are in the "settings" block below.
*/

#include <Wire.h>
#include <Preferences.h>
#include <math.h>
#include "board.h"
#include "esp_lcd_touch_axs5106l.h"

// ================================ settings =================================
// 1 or 3. If the face is upside down in your case (ears at the bottom), use
// the other one.
#define EYE_ROTATION   3

#define LCD_BRIGHTNESS 150   // backlight, 0 (off) .. 255 (full)

#define SCREEN_W       320
#define SCREEN_H       172

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// The looks, in the order they cycle. All are eyes only, on black.
enum Style {
  STYLE_GOOGLY, STYLE_CAT, STYLE_ROBOT, STYLE_SOFT,
  STYLE_HEART, STYLE_SLEEPY, STYLE_SPARKLE, STYLE_PIXEL,
  STYLE_SAVAGE, STYLE_CUTE
};

struct Look {
  Style style;
  uint16_t bg, eye;
  uint16_t rim = 0;   // darker edge colour (savage eyes only)
};

static const Look LOOKS[] = {
  // 1. Cartoon eyes: white, with big dark pupils and a sparkle.
  {STYLE_GOOGLY, rgb565(0, 0, 0), rgb565(255, 255, 255)},
  // 2. Cat eyes: green, with slit pupils.
  {STYLE_CAT, rgb565(0, 0, 0), rgb565(118, 224, 0)},
  // 3. Robot pet: two glowing blue blocks.
  {STYLE_ROBOT, rgb565(0, 0, 0), rgb565(70, 220, 255)},
  // 4. Plain white ovals.
  {STYLE_SOFT, rgb565(0, 0, 0), rgb565(255, 255, 255)},
  // 5. Heart eyes: two pink hearts.
  {STYLE_HEART, rgb565(0, 0, 0), rgb565(255, 70, 120)},
  // 6. Sleepy eyes: white, half covered by heavy eyelids.
  {STYLE_SLEEPY, rgb565(0, 0, 0), rgb565(255, 255, 255)},
  // 7. Sparkle eyes: white, with big blue irises and sparkles.
  {STYLE_SPARKLE, rgb565(0, 0, 0), rgb565(255, 255, 255)},
  // 8. Pixel eyes: chunky retro blocks.
  {STYLE_PIXEL, rgb565(0, 0, 0), rgb565(255, 255, 255)},
  // 9-12. Savage eyes: narrowed, slanted glare with slit pupils, in amber,
  // red, green and ice blue. Touching the screen makes them flash red.
  {STYLE_SAVAGE, rgb565(0, 0, 0), rgb565(255, 205, 0), rgb565(255, 110, 0)},
  {STYLE_SAVAGE, rgb565(0, 0, 0), rgb565(255, 45, 35), rgb565(190, 0, 0)},
  {STYLE_SAVAGE, rgb565(0, 0, 0), rgb565(120, 255, 60), rgb565(0, 150, 60)},
  {STYLE_SAVAGE, rgb565(0, 0, 0), rgb565(120, 220, 255), rgb565(0, 110, 255)},
  // 13. Cute eyes in the savage amber: big round eyes, wide pupils, sparkles.
  {STYLE_CUTE, rgb565(0, 0, 0), rgb565(255, 205, 0), rgb565(255, 110, 0)},
  // 14. Half eyes (like the sleepy ones, look 6) in the same amber.
  {STYLE_SLEEPY, rgb565(0, 0, 0), rgb565(255, 205, 0), rgb565(255, 110, 0)},
};
#define LOOK_COUNT     (sizeof(LOOKS) / sizeof(LOOKS[0]))
#define DEFAULT_LOOK   13 // look used until you pick another (0 = first)
#define LOOK_HOLD_MS   1500  // hold this long to switch look (0 = never)
// Slideshow: move on to the next look after this many crossings of the
// screen (2 = once left-to-right and back). 0 = stay on one look.
#define LOOK_EVERY_SWEEPS 0
#define BOOT_BTN       8     // BOOT button: a press also switches look

#define SWEEP_MS       2500// time to travel from one side to the other (bigger = slower)
#define HOLD_MS        500  // pause at each side; the blink happens here
#define HAPPY_MS       1200 // how long the happy eyes stay after a touch

#define FRAME_MS       25   // minimum time per frame (25 ms = 40 fps cap)
// ===========================================================================

static const Look *look = &LOOKS[DEFAULT_LOOK];

static uint16_t *fb = nullptr;           // SCREEN_W * SCREEN_H pixels
static uint16_t cov[SCREEN_W];           // per-row coverage, 0..256

// Rows drawn in the previous frame (to erase and re-send) and in this one.
static int16_t prevTop = 0;
static int16_t prevBot = SCREEN_H - 1;
static int16_t drawTop = SCREEN_H;
static int16_t drawBot = -1;

// ------------------------------- rendering ---------------------------------

// An ellipse (corner < 0), or a rounded rectangle with that corner radius.
// rx / ry are the half width and half height either way.
struct Shape {
  float cx, cy, rx, ry, corner;
};

static inline Shape oval(float cx, float cy, float rx, float ry) {
  return {cx, cy, rx, ry, -1.0f};
}

static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint32_t a /*0..256*/) {
  const uint32_t ia = 256 - a;
  const uint32_t rb = (((fg & 0xF81F) * a + (bg & 0xF81F) * ia) >> 8) & 0xF81F;
  const uint32_t g  = (((fg & 0x07E0) * a + (bg & 0x07E0) * ia) >> 8) & 0x07E0;
  return rb | g;
}

// Adds one quarter-row's worth of coverage for the span [xl, xr).
// A fully covered pixel gains 64, so four sub-rows add up to 256.
static void addSpan(float xl, float xr) {
  if (xl < 0) xl = 0;
  if (xr > SCREEN_W) xr = SCREEN_W;
  if (xr <= xl) return;

  const int32_t l = (int32_t)(xl * 64.0f);
  const int32_t r = (int32_t)(xr * 64.0f);
  const int32_t il = l >> 6;
  const int32_t ir = r >> 6;

  if (il == ir) {
    cov[il] += r - l;
    return;
  }
  cov[il] += 64 - (l & 63);
  for (int32_t i = il + 1; i < ir; i++) cov[i] += 64;
  if (ir < SCREEN_W) cov[ir] += r & 63;
}

// Half width of a shape at height sy, or a negative number if sy misses it.
static inline float halfWidthAt(const Shape &s, float sy) {
  if (s.corner < -1.5f) {
    // Pointed bottom of a heart: full width at cy, narrowing to a tip ry below.
    const float t = (sy - s.cy) / s.ry;
    if (t < 0 || t >= 1.0f) return -1.0f;
    return s.rx * (1.0f - powf(t, 1.6f));
  }
  const float dy = fabsf(sy - s.cy);
  if (dy >= s.ry) return -1.0f;
  if (s.corner < 0) {
    const float d = dy / s.ry;
    return s.rx * sqrtf(1.0f - d * d);
  }
  float r = s.corner;
  if (r > s.rx) r = s.rx;
  if (r > s.ry) r = s.ry;
  const float t = dy - (s.ry - r);          // how far into the rounded part
  if (t <= 0) return s.rx;
  return s.rx - r + sqrtf(r * r - t * t);
}

// Fills a smooth-edged shape into fb. If clip is given, only the part inside
// that shape is drawn.
static void fillShape(const Shape &s, uint16_t colour, const Shape *clip = nullptr) {
  if (s.rx < 0.5f || s.ry < 0.5f) return;

  int16_t y0 = (int16_t)floorf(s.cy - s.ry);
  int16_t y1 = (int16_t)ceilf(s.cy + s.ry);
  if (y0 < 0) y0 = 0;
  if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;

  int16_t x0 = (int16_t)floorf(s.cx - s.rx) - 1;
  int16_t x1 = (int16_t)ceilf(s.cx + s.rx) + 1;
  if (x0 < 0) x0 = 0;
  if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
  if (x1 < x0 || y1 < y0) return;

  if (y0 < drawTop) drawTop = y0;
  if (y1 > drawBot) drawBot = y1;

  for (int16_t y = y0; y <= y1; y++) {
    memset(&cov[x0], 0, (x1 - x0 + 1) * sizeof(uint16_t));

    // Four sub-rows per pixel row smooth the top and bottom edges.
    for (int i = 0; i < 4; i++) {
      const float sy = y + 0.125f + 0.25f * i;
      const float hw = halfWidthAt(s, sy);
      if (hw < 0) continue;
      float xl = s.cx - hw;
      float xr = s.cx + hw;
      if (clip) {
        const float chw = halfWidthAt(*clip, sy);
        if (chw < 0) continue;
        if (xl < clip->cx - chw) xl = clip->cx - chw;
        if (xr > clip->cx + chw) xr = clip->cx + chw;
      }
      addSpan(xl, xr);
    }

    uint16_t *row = fb + (int32_t)y * SCREEN_W;
    for (int16_t x = x0; x <= x1; x++) {
      uint32_t c = cov[x];
      if (c == 0) continue;
      if (c > 256) c = 256;
      row[x] = blend565(row[x], colour, c);
    }
  }
}

// A curved stroke: the shape minus a copy of itself shifted by dy.
// dy > 0 leaves an arc on top ("^"), dy < 0 leaves one underneath ("u").
static void fillArc(float cx, float cy, float rx, float ry, float dy, uint16_t colour) {
  fillShape(oval(cx, cy, rx, ry), colour);
  fillShape(oval(cx, cy + dy, rx + 2.0f, ry), look->bg);
}

// Paints the part of `area` that lies on one side of a slanted line through
// (px, py). slope is how many pixels the line drops per pixel to the right.
// below = false paints above the line, true paints below it. Used for the
// angled eyelids of the savage eyes.
static void fillSlant(const Shape &area, float px, float py, float slope, bool below,
                      uint16_t colour) {
  if (fabsf(slope) < 0.0005f) return;

  int16_t y0 = (int16_t)floorf(area.cy - area.ry);
  int16_t y1 = (int16_t)ceilf(area.cy + area.ry);
  if (y0 < 0) y0 = 0;
  if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;
  int16_t x0 = (int16_t)floorf(area.cx - area.rx) - 2;
  int16_t x1 = (int16_t)ceilf(area.cx + area.rx) + 2;
  if (x0 < 0) x0 = 0;
  if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
  if (x1 < x0 || y1 < y0) return;

  bool rightSide = slope > 0;             // which side of the line gets painted
  if (below) rightSide = !rightSide;

  for (int16_t y = y0; y <= y1; y++) {
    memset(&cov[x0], 0, (x1 - x0 + 1) * sizeof(uint16_t));
    for (int i = 0; i < 4; i++) {
      const float sy = y + 0.125f + 0.25f * i;
      const float hw = halfWidthAt(area, sy);
      if (hw < 0) continue;
      float xl = area.cx - hw - 1.0f;
      float xr = area.cx + hw + 1.0f;
      const float xLine = px + (sy - py) / slope;
      if (rightSide) {
        if (xl < xLine) xl = xLine;
      } else {
        if (xr > xLine) xr = xLine;
      }
      if (xl < x0) xl = x0;
      if (xr > x1 + 1) xr = x1 + 1;
      addSpan(xl, xr);
    }
    uint16_t *row = fb + (int32_t)y * SCREEN_W;
    for (int16_t x = x0; x <= x1; x++) {
      uint32_t c = cov[x];
      if (c == 0) continue;
      if (c > 256) c = 256;
      row[x] = blend565(row[x], colour, c);
    }
  }
}

// Pure black: on this panel, very dark greys come out visibly blue.
static const uint16_t COL_PUPIL = rgb565(0, 0, 0);
static const uint16_t COL_SHINE = rgb565(255, 255, 255);

// Cartoon eye: white ball, big dark pupil that moves, two sparkles.
static void drawGooglyEye(float cx, float cy, float gazeX, float k) {
  const Shape eye = oval(cx, cy, 50.0f, 62.0f * k);
  fillShape(eye, look->eye);
  const float px = cx + gazeX * 16.0f;
  const float py = cy + 4.0f * k;
  fillShape(oval(px, py, 30.0f, 30.0f * k), COL_PUPIL, &eye);
  fillShape(oval(px - 10.0f, py - 11.0f * k, 10.0f, 10.0f * k), COL_SHINE, &eye);
  fillShape(oval(px + 11.0f, py + 12.0f * k, 4.5f, 4.5f * k), COL_SHINE, &eye);
}

// Cat eye: green ball, slit pupil that moves, one sparkle.
static void drawCatEye(float cx, float cy, float gazeX, float k) {
  const Shape eye = oval(cx, cy, 54.0f, 64.0f * k);
  fillShape(eye, look->eye);
  const float px = cx + gazeX * 20.0f;
  fillShape(oval(px, cy, 13.0f, 40.0f * k), look->bg, &eye);
  fillShape(oval(cx - 20.0f, cy - 26.0f * k, 9.0f, 10.0f * k), COL_SHINE, &eye);
}

// Draws both eyes for the current look.
static void drawEyes(float gazeX, float open, bool happy) {
  const float mid = SCREEN_W * 0.5f;
  const float cy = 86.0f;
  for (int side = -1; side <= 1; side += 2) {
    switch (look->style) {
      case STYLE_ROBOT: {
        const float cx = mid + side * 66.0f + gazeX * 18.0f;
        if (happy) {
          const Shape block = {cx, 80.0f, 42.0f, 36.0f, 32.0f};
          fillShape(block, look->eye);
          fillShape(oval(cx, 128.0f, 64.0f, 44.0f), look->bg);  // scoop out the bottom
        } else {
          float hh = 58.0f * open;
          if (hh < 5.0f) hh = 5.0f;                             // shut = a bar
          const Shape block = {cx, cy, 40.0f + 2.0f * (1.0f - open), hh, 28.0f};
          fillShape(block, look->eye);
        }
        break;
      }
      case STYLE_SOFT: {
        const float cx = mid + side * 62.0f + gazeX * 18.0f;
        if (happy) {
          fillArc(cx, cy + 8.0f, 40.0f, 38.0f, 14.0f, look->eye);
        } else if (open < 0.2f) {
          fillArc(cx, cy + 4.0f, 38.0f, 18.0f, -6.0f, look->eye);
        } else {
          fillShape(oval(cx, cy, 34.0f, 56.0f * open), look->eye);
        }
        break;
      }
      case STYLE_CAT: {
        const float cx = mid + side * 72.0f + gazeX * 8.0f;
        if (happy) {
          fillArc(cx, cy + 10.0f, 54.0f, 46.0f, 16.0f, look->eye);
        } else if (open < 0.2f) {
          fillArc(cx, cy + 4.0f, 52.0f, 22.0f, -7.0f, look->eye);
        } else {
          drawCatEye(cx, cy, gazeX, open);
        }
        break;
      }
      case STYLE_HEART: {
        const float cx = mid + side * 72.0f + gazeX * 18.0f;
        if (happy) {
          fillArc(cx, 94.0f, 50.0f, 44.0f, 16.0f, look->eye);
        } else if (open < 0.2f) {
          fillArc(cx, 88.0f, 48.0f, 22.0f, -7.0f, look->eye);
        } else {
          // Two round lobes on top of a pointed bottom, plus a soft shine.
          const float k = open;
          const float r = 27.0f;
          const float top = 84.0f - 22.0f * k;
          fillShape(oval(cx - r * 0.93f, top, r, r * k), look->eye);
          fillShape(oval(cx + r * 0.93f, top, r, r * k), look->eye);
          const Shape point = {cx, top, r * 1.93f, 74.0f * k, -2.0f};
          fillShape(point, look->eye);
          fillShape(oval(cx - r * 1.15f, top - 9.0f * k, 7.0f, 9.0f * k), rgb565(255, 200, 215));
        }
        break;
      }
      case STYLE_SLEEPY: {
        const float cx = mid + side * 70.0f + gazeX * 8.0f;
        const float ey = 62.0f;           // high up, so the part below the lid sits mid-screen
        if (happy) {
          fillArc(cx, ey + 6.0f, 50.0f, 44.0f, 16.0f, look->eye);
        } else if (open < 0.2f) {
          fillArc(cx, ey + 4.0f, 48.0f, 22.0f, -7.0f, look->eye);
        } else {
          const float k = open;
          const Shape eye = oval(cx, ey, 50.0f, 56.0f * k);
          if (look->rim) {
            // Two-tone version: darker edge, lighter middle.
            fillShape(eye, look->rim);
            fillShape(oval(cx, ey + 2.0f * k, 42.0f, 48.0f * k), look->eye);
          } else {
            fillShape(eye, look->eye);
          }
          const float px = cx + gazeX * 16.0f;
          const float py = ey + 16.0f * k;
          fillShape(oval(px, py, 24.0f, 24.0f * k), COL_PUPIL, &eye);
          fillShape(oval(px - 8.0f, py - 6.0f * k, 6.0f, 6.0f * k), COL_SHINE, &eye);
          const Shape lid = {cx, ey - 36.0f * k, 56.0f, 32.0f * k, 0.0f};
          fillShape(lid, look->bg);       // flat, heavy eyelid
        }
        break;
      }
      case STYLE_SPARKLE: {
        const float cx = mid + side * 70.0f + gazeX * 8.0f;
        if (happy) {
          fillArc(cx, cy + 10.0f, 52.0f, 44.0f, 16.0f, look->eye);
        } else if (open < 0.2f) {
          fillArc(cx, cy + 4.0f, 50.0f, 22.0f, -7.0f, look->eye);
        } else {
          const float k = open;
          const Shape eye = oval(cx, cy, 50.0f, 62.0f * k);
          fillShape(eye, look->eye);
          const float px = cx + gazeX * 13.0f;
          const float py = cy + 4.0f * k;
          fillShape(oval(px, py, 36.0f, 40.0f * k), rgb565(40, 90, 225), &eye);
          fillShape(oval(px, py + 8.0f * k, 28.0f, 28.0f * k), rgb565(90, 180, 255), &eye);
          fillShape(oval(px, py, 17.0f, 20.0f * k), COL_PUPIL, &eye);
          fillShape(oval(px - 13.0f, py - 16.0f * k, 11.0f, 12.0f * k), COL_SHINE, &eye);
          fillShape(oval(px + 13.0f, py + 15.0f * k, 5.0f, 5.0f * k), COL_SHINE, &eye);
        }
        break;
      }
      case STYLE_PIXEL: {
        // Everything is built from 12-pixel blocks and moves a block at a time.
        const float P = 12.0f;
        const float cx = mid + side * 66.0f;
        const int g = gazeX < -0.33f ? -1 : gazeX > 0.33f ? 1 : 0;
        auto block = [](float x, float y, float hw, float hh, uint16_t colour) {
          const Shape b = {x, y, hw, hh, 0.0f};
          fillShape(b, colour);
        };
        if (happy) {
          static const int8_t dots[5][2] = {{-2, 1}, {-1, 0}, {0, -1}, {1, 0}, {2, 1}};
          for (int i = 0; i < 5; i++) {
            block(cx + dots[i][0] * P, cy + dots[i][1] * P, P / 2, P / 2, look->eye);
          }
        } else if (open < 0.5f) {
          block(cx, cy + P / 2, P * 3, P / 2, look->eye);
        } else {
          block(cx, cy, P * 3, P * 3.5f, look->eye);
          for (int ex = -1; ex <= 1; ex += 2) {
            for (int ey = -1; ey <= 1; ey += 2) {
              block(cx + ex * P * 2.5f, cy + ey * P * 3, P / 2, P / 2, look->bg);  // clipped corners
            }
          }
          const float px = cx + g * P;
          block(px, cy + P / 2, P, P * 1.5f, look->bg);          // pupil
          block(px - P / 2, cy - P / 2, P / 2, P / 2, look->eye); // sparkle
        }
        break;
      }
      case STYLE_SAVAGE: {
        // A wide eye with a darker edge and a slit pupil, glaring from under
        // an eyelid that slants down towards the middle of the face. When
        // touched (happy), it turns red and narrows further.
        const float cx = mid + side * 74.0f + gazeX * 10.0f;
        const float ey = 92.0f;
        const float k = open < 0.08f ? 0.08f : open;
        const uint16_t rim = happy ? rgb565(200, 0, 0) : look->rim;
        const uint16_t core = happy ? rgb565(255, 40, 30) : look->eye;

        const Shape eye = oval(cx, ey, 60.0f, 48.0f * k);
        fillShape(eye, rim);
        fillShape(oval(cx, ey + 2.0f * k, 50.0f, 38.0f * k), core);

        const float px = cx + gazeX * 18.0f - side * 6.0f;   // pupils turned slightly inwards
        fillShape(oval(px, ey + 4.0f * k, happy ? 4.0f : 7.0f, 34.0f * k), look->bg, &eye);

        const Shape area = oval(cx, ey, 64.0f, 52.0f * k);
        fillSlant(area, cx, ey - (happy ? 6.0f : 16.0f) * k, -side * 0.42f * k, false, look->bg);
        fillSlant(area, cx, ey + (happy ? 30.0f : 40.0f) * k, side * 0.12f * k, true, look->bg);
        break;
      }
      case STYLE_CUTE: {
        // Big round two-tone eye with a wide, tall pupil and two sparkles.
        const float cx = mid + side * 72.0f + gazeX * 10.0f;
        if (happy) {
          fillArc(cx, cy + 10.0f, 54.0f, 46.0f, 16.0f, look->eye);
        } else if (open < 0.2f) {
          fillArc(cx, cy + 4.0f, 52.0f, 22.0f, -7.0f, look->eye);
        } else {
          const float k = open;
          const Shape eye = oval(cx, cy, 56.0f, 64.0f * k);
          fillShape(eye, look->rim);
          fillShape(oval(cx, cy + 2.0f * k, 47.0f, 55.0f * k), look->eye);
          const float px = cx + gazeX * 14.0f;
          const float py = cy + 4.0f * k;
          fillShape(oval(px, py, 24.0f, 38.0f * k), look->bg, &eye);
          fillShape(oval(px - 9.0f, py - 16.0f * k, 9.0f, 11.0f * k), COL_SHINE, &eye);
          fillShape(oval(px + 9.0f, py + 17.0f * k, 4.0f, 4.5f * k), COL_SHINE, &eye);
        }
        break;
      }
      default: {  // STYLE_GOOGLY
        const float cx = mid + side * 70.0f + gazeX * 8.0f;
        if (happy) {
          fillArc(cx, cy + 10.0f, 52.0f, 44.0f, 16.0f, look->eye);
        } else if (open < 0.2f) {
          fillArc(cx, cy + 4.0f, 50.0f, 22.0f, -7.0f, look->eye);
        } else {
          drawGooglyEye(cx, cy, gazeX, open);
        }
        break;
      }
    }
  }
}

// gazeX: -1..1 (left..right). open: 0 = shut, 1 = wide open.
static void renderFrame(float gazeX, float open, bool happy) {
  // Erase what the previous frame drew.
  for (int32_t i = (int32_t)prevTop * SCREEN_W; i < (int32_t)(prevBot + 1) * SCREEN_W; i++) {
    fb[i] = look->bg;
  }
  drawTop = SCREEN_H;
  drawBot = -1;

  drawEyes(gazeX, open, happy);
  if (drawBot < drawTop) { drawTop = 0; drawBot = 0; }

  // Send only the rows that changed: last frame's drawing plus this frame's.
  const int16_t sendTop = min(drawTop, prevTop);
  const int16_t sendBot = max(drawBot, prevBot);
  gfx->draw16bitRGBBitmap(0, sendTop, fb + (int32_t)sendTop * SCREEN_W,
                          SCREEN_W, sendBot - sendTop + 1);
  prevTop = drawTop;
  prevBot = drawBot;
}

// ------------------------------- animation ---------------------------------

static float smooth(float t) {            // ease in and out, 0..1
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  return t * t * (3.0f - 2.0f * t);
}

// Moves cur towards target; rate is "fraction of the gap closed per second".
static float approach(float cur, float target, float rate, float dt) {
  float k = rate * dt;
  if (k > 1) k = 1;
  return cur + (target - cur) * k;
}

static float gazeX = 0;                   // where the face is looking now
static float gazeTX = 0;                  // where it is heading
static int sweepDir = 1;                  // +1 = heading right, -1 = heading left
static uint32_t sweepStart = 0;
static bool sweepBlinked = false;

static uint32_t blinkStart = 0;
static bool blinking = false;

static uint32_t lastTouchAt = 0;
static uint32_t touchBeganAt = 0;
static bool happy = false;                // being petted
static bool lookSwitched = false;         // already switched during this hold

static Preferences prefs;
static uint8_t lookIndex = DEFAULT_LOOK;

#define BLINK_CLOSE_MS  80
#define BLINK_HOLD_MS   60
#define BLINK_OPEN_MS   140

static float blinkOpenness(uint32_t now) {
  if (!blinking) return 1.0f;
  const uint32_t t = now - blinkStart;
  if (t < BLINK_CLOSE_MS) return 1.0f - smooth((float)t / BLINK_CLOSE_MS);
  if (t < BLINK_CLOSE_MS + BLINK_HOLD_MS) return 0.0f;
  if (t < BLINK_CLOSE_MS + BLINK_HOLD_MS + BLINK_OPEN_MS) {
    return smooth((float)(t - BLINK_CLOSE_MS - BLINK_HOLD_MS) / BLINK_OPEN_MS);
  }
  blinking = false;
  return 1.0f;
}

static void startBlink(uint32_t now) {
  blinking = true;
  blinkStart = now;
}

// Switches look and repaints the whole screen on the next frame.
static void setLook(uint8_t index) {
  lookIndex = index % LOOK_COUNT;
  look = &LOOKS[lookIndex];
  for (int32_t i = 0; i < (int32_t)SCREEN_W * SCREEN_H; i++) fb[i] = look->bg;
  prevTop = 0;
  prevBot = SCREEN_H - 1;
}

static void nextLook() {
  setLook(lookIndex + 1);
  prefs.putUChar("eyelook5", lookIndex);
  Serial.printf("look %u\n", lookIndex + 1);
}

static void pollTouch(uint32_t now) {
  // The BOOT button also switches look, in case touch is hard to reach.
  static bool bootWasDown = false;
  const bool bootDown = digitalRead(BOOT_BTN) == LOW;
  if (bootDown && !bootWasDown) nextLook();
  bootWasDown = bootDown;

  // Ask the touch chip every frame instead of waiting for its interrupt
  // line: the driver only reads when this flag is set, and the interrupt
  // may not keep firing while a finger rests still on the glass.
  extern bool g_touch_int_flag;
  g_touch_int_flag = true;

  touch_data_t touch;
  bsp_touch_read();
  if (bsp_touch_get_coordinates(&touch) && touch.touch_num > 0) {
    // A gap of more than 600 ms means the finger was lifted and put back.
    if (!happy || now - lastTouchAt > 600) {
      touchBeganAt = now;
      lookSwitched = false;
      Serial.println("touch down");
    }
    if (!happy) {
      happy = true;
      startBlink(now);                    // eyes close, then turn into arcs
    }
    lastTouchAt = now;
    if (LOOK_HOLD_MS > 0 && !lookSwitched && now - touchBeganAt >= LOOK_HOLD_MS) {
      lookSwitched = true;
      nextLook();
    }
  } else if (happy && now - lastTouchAt > HAPPY_MS) {
    // Petting stopped: open the eyes again from shut and go back to idling.
    Serial.println("touch up");
    happy = false;
    blinking = true;
    blinkStart = now - (BLINK_CLOSE_MS + BLINK_HOLD_MS);
    sweepStart = now;
    sweepBlinked = false;
  }
}

static int sweepsDone = 0;                // crossings since the look last changed
static bool lookChangePending = false;    // swap look once the eyes are shut

static void idle(uint32_t now) {
  if (happy) {
    gazeTX = 0;                           // face forward while being petted
    return;
  }

  // Look to one side, pause and blink, then look to the other.
  const uint32_t t = now - sweepStart;
  if (t < SWEEP_MS) {
    gazeTX = sweepDir * (2.0f * smooth((float)t / SWEEP_MS) - 1.0f);
  } else {
    gazeTX = sweepDir;
    const uint32_t blinkMs = BLINK_CLOSE_MS + BLINK_HOLD_MS + BLINK_OPEN_MS;
    const uint32_t blinkAt = SWEEP_MS + (HOLD_MS > blinkMs ? (HOLD_MS - blinkMs) / 2 : 0);
    if (!sweepBlinked && t >= blinkAt) {
      startBlink(now);
      sweepBlinked = true;
      // Slideshow of looks: after enough crossings, swap during this blink.
      if (LOOK_EVERY_SWEEPS > 0 && ++sweepsDone >= LOOK_EVERY_SWEEPS) {
        sweepsDone = 0;
        lookChangePending = true;
      }
    }
    if (t >= SWEEP_MS + HOLD_MS && !blinking) {
      sweepDir = -sweepDir;
      sweepStart = now;
      sweepBlinked = false;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("Eyes starting");

  if (!board_lcd_begin()) {
    Serial.println("gfx->begin() failed");
  }
  gfx->setRotation(EYE_ROTATION);

  // Dim the backlight a little: it makes the black background darker, so
  // the lit rectangle of the screen stands out less inside the case.
  ledcAttach(LCD_BL, 5000, 8);
  ledcWrite(LCD_BL, BL_ON == HIGH ? LCD_BRIGHTNESS : 255 - LCD_BRIGHTNESS);

  pinMode(BOOT_BTN, INPUT_PULLUP);
  prefs.begin("eyes", false);
  lookIndex = prefs.getUChar("eyelook5", DEFAULT_LOOK) % LOOK_COUNT;
  look = &LOOKS[lookIndex];
  gfx->fillScreen(look->bg);

  fb = (uint16_t *)malloc((size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t));
  if (!fb) {
    Serial.println("not enough memory for the frame buffer");
    gfx->setTextColor(RGB565_RED);
    gfx->setCursor(10, 10);
    gfx->print("out of memory");
    while (true) delay(1000);
  }
  for (int32_t i = 0; i < (int32_t)SCREEN_W * SCREEN_H; i++) fb[i] = look->bg;

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  bsp_touch_init(&Wire, TOUCH_RST, TOUCH_INT,
                 gfx->getRotation(), gfx->width(), gfx->height());
}

void loop() {
  static uint32_t lastFrame = millis();
  const uint32_t now = millis();
  float dt = (now - lastFrame) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  lastFrame = now;

  pollTouch(now);
  idle(now);

  gazeX = approach(gazeX, gazeTX, 12.0f, dt);

  const float open = blinkOpenness(now);
  // The look changes while the eyes are shut, so the swap is hidden by the blink.
  if (lookChangePending && (!blinking || now - blinkStart >= BLINK_CLOSE_MS)) {
    lookChangePending = false;
    setLook(lookIndex + 1);
  }
  // The arcs appear once the eyes have finished closing.
  const bool arcs = happy && (!blinking || now - blinkStart >= BLINK_CLOSE_MS);
  renderFrame(gazeX, open, arcs);

  const uint32_t took = millis() - now;
  if (took < FRAME_MS) delay(FRAME_MS - took);
}
