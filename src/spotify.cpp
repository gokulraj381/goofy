/*
  Spotify "now playing" for Waveshare ESP32-C6-Touch-LCD-1.47 (landscape)

  Shows the song playing on your Spotify account: album cover, title, artist
  and a progress bar. It asks Spotify every few seconds over WiFi, so it
  follows whatever device you are listening on.

  Touch the screen to control playback: left third = previous song,
  right third = next song, middle = pause / play. Hold the middle for a
  moment to see the songs coming up next.

  Setup (once):  python tools\spotify_login.py
  That script writes include/secrets.h (WiFi + Spotify keys) and can upload.

  Notes:
    - The board needs internet. Away from home, use your phone's hotspot (the
      login script lets you add it as a second network).
    - Text uses a plain Latin font. Letters it does not have (for example
      Tamil or Hindi script) are shown as "?".
    - HTTPS certificates are not checked (see setInsecure below), which keeps
      the firmware small and simple. Someone controlling the WiFi network
      could in theory read the Spotify key, which only allows seeing what is
      playing.
*/

#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <Wire.h>
#include "board.h"
#include "esp_lcd_touch_axs5106l.h"
#include "secrets.h"
#include "FreeSansBold10pt7b.h"

// ================================ settings =================================
// 1 or 3. If the picture is upside down in your case, use the other one.
#define SCREEN_ROTATION  3

#define POLL_PLAYING_MS  3000   // how often to ask Spotify while a song plays
#define POLL_IDLE_MS     5000   // ... and while nothing is playing
// ===========================================================================

#define SCREEN_W   320
#define SCREEN_H   172

// Layout
#define ART_X      10
#define ART_Y      11
#define ART_SIZE   150
#define TEXT_X     172
#define TEXT_W     140
#define LINE_H     24
#define FIRST_BASELINE 30
#define BAR_Y      122
#define BAR_H      6
#define TIME_Y     136
#define ART_MAX_BYTES 90000

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static const uint16_t COL_BG    = rgb565(0, 0, 0);
static const uint16_t COL_TEXT  = rgb565(255, 255, 255);
static const uint16_t COL_DIM   = rgb565(170, 170, 170);
static const uint16_t COL_GREEN = rgb565(30, 215, 96);
static const uint16_t COL_TRACK = rgb565(60, 60, 60);
static const uint16_t COL_NOART = rgb565(34, 34, 34);

struct Track {
  String id, title, artist, artUrl;
  uint32_t durationMs = 0;
};

static JPEGDEC jpeg;

static String accessToken;
static uint32_t tokenGoodUntil = 0;       // millis() when the token must be renewed

static Track cur;                         // the song on screen
static bool isPlaying = false;
static uint32_t progressMs = 0;           // position Spotify last reported ...
static uint32_t progressAt = 0;           // ... and when (millis)

static String curContext;                 // playlist/album the song is playing from ("" = none)

enum Screen { SCREEN_NONE, SCREEN_MESSAGE, SCREEN_TRACK, SCREEN_QUEUE };
static Screen screen = SCREEN_NONE;
static String shownMessage;

// --------------------------------- text ------------------------------------

// The font only has plain Latin letters. Turns accented letters into plain
// ones, fancy punctuation into simple punctuation, and anything else into "?".
static String toPlainText(const char *utf8) {
  static const char LATIN1[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  String out;
  const uint8_t *p = (const uint8_t *)utf8;
  while (*p) {
    uint32_t cp;
    if (*p < 0x80) {
      cp = *p++;
    } else {
      int extra = (*p >= 0xF0) ? 3 : (*p >= 0xE0) ? 2 : (*p >= 0xC0) ? 1 : 0;
      cp = (extra == 3) ? (*p & 0x07) : (extra == 2) ? (*p & 0x0F) : (*p & 0x1F);
      p++;
      while (extra-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    }

    if (cp >= 0x20 && cp < 0x7F) {
      out += (char)cp;
    } else if (cp >= 0xC0 && cp <= 0xFF) {
      out += LATIN1[cp - 0xC0];
    } else if (cp == 0x2018 || cp == 0x2019) {
      out += '\'';
    } else if (cp == 0x201C || cp == 0x201D) {
      out += '"';
    } else if (cp == 0x2013 || cp == 0x2014) {
      out += '-';
    } else if (cp == 0x2026) {
      out += "...";
    } else if (cp == 0xA0 || cp < 0x20) {
      out += ' ';
    } else if (!out.endsWith("?")) {
      out += '?';                         // one "?" for a run of unknown letters
    }
  }
  out.trim();
  return out;
}

static uint16_t textWidth(const String &s) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// Shortens s until it fits in maxW, ending it with "..." if it was cut.
static String fitWithDots(String s, int maxW) {
  if (textWidth(s) <= maxW) return s;
  while (s.length() > 1 && textWidth(s + "...") > maxW) s.remove(s.length() - 1);
  s.trim();
  return s + "...";
}

// Splits text into at most maxLines lines no wider than maxW. Returns the
// number of lines used; the last one ends in "..." if text was left over.
static int wrapText(const String &text, int maxW, int maxLines, String *lines) {
  const int len = text.length();
  int n = 0;
  int pos = 0;
  String line;
  while (pos < len && n < maxLines) {
    int sp = text.indexOf(' ', pos);
    if (sp < 0) sp = len;
    const String word = text.substring(pos, sp);
    const String candidate = line.length() ? line + " " + word : word;
    if (textWidth(candidate) <= maxW) {
      line = candidate;
      pos = sp + 1;
    } else if (line.length() == 0) {
      // A single word wider than the line: cut it where it fits.
      int k = word.length();
      while (k > 1 && textWidth(word.substring(0, k)) > maxW) k--;
      lines[n++] = word.substring(0, k);
      pos += k;
    } else {
      lines[n++] = line;
      line = "";
    }
  }
  if (line.length() && n < maxLines) {
    lines[n++] = line;
  } else if (n > 0 && (pos < len || line.length())) {
    lines[n - 1] = fitWithDots(lines[n - 1] + " " + line + text.substring(min(pos, len)), maxW);
  }
  return n;
}

// -------------------------------- drawing ----------------------------------

// A centred message for when there is no song to show.
static void showMessage(const String &big, const String &small) {
  const String key = big + "|" + small;
  if (screen == SCREEN_MESSAGE && key == shownMessage) return;
  screen = SCREEN_MESSAGE;
  shownMessage = key;
  cur = Track();

  gfx->fillScreen(COL_BG);
  gfx->setFont(&FreeSansBold10pt7b);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_TEXT);
  gfx->setCursor((SCREEN_W - (int)textWidth(big)) / 2, 82);
  gfx->print(big);

  gfx->setFont();
  gfx->setTextColor(COL_DIM);
  gfx->setCursor((SCREEN_W - (int)small.length() * 6) / 2, 100);
  gfx->print(small);
}

static void touchSample();                // defined with the touch code below

static int jpegDraw(JPEGDRAW *d) {
  gfx->draw16bitRGBBitmap(d->x, d->y, d->pPixels, d->iWidth, d->iHeight);
  return 1;
}

// Downloads the album cover and draws it. Returns false if that failed
// (the grey placeholder square then stays).
// The picture is centred in the square at (boxX, boxY) with side boxSize.
static bool drawCover(const String &url, int boxX, int boxY, int boxSize) {
  if (url.length() == 0) return false;

  // One connection to the picture server is kept open, so several covers in
  // a row (the "up next" list) load quickly.
  static NetworkClientSecure client;
  static HTTPClient http;
  static bool ready = false;
  if (!ready) {
    client.setInsecure();
    http.setReuse(true);
    http.setTimeout(8000);
    ready = true;
  }
  if (!http.begin(client, url)) return false;
  const int code = http.GET();
  const int len = http.getSize();
  if (code != 200 || len <= 0 || len > ART_MAX_BYTES) {
    Serial.printf("cover: http %d, %d bytes\n", code, len);
    http.end();
    return false;
  }

  uint8_t *buf = (uint8_t *)malloc(len);
  if (!buf) {
    http.end();
    return false;
  }
  NetworkClient *stream = http.getStreamPtr();
  int got = 0;
  uint32_t lastData = millis();
  while (got < len && millis() - lastData < 8000) {
    const int n = stream->read(buf + got, len - got);
    if (n > 0) {
      got += n;
      lastData = millis();
    } else if (!http.connected()) {
      break;
    } else {
      delay(5);
    }
    touchSample();                        // keep noticing touches while downloading
  }
  http.end();

  bool ok = false;
  if (got == len && jpeg.openRAM(buf, len, jpegDraw)) {
    // Shrink by 2, 4 or 8 so the cover fits the square.
    int div = 1;
    while (jpeg.getWidth() / div > boxSize && div < 8) div *= 2;
    const int opt = div == 2 ? JPEG_SCALE_HALF : div == 4 ? JPEG_SCALE_QUARTER
                  : div == 8 ? JPEG_SCALE_EIGHTH : 0;
    const int w = jpeg.getWidth() / div;
    const int h = jpeg.getHeight() / div;
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    ok = jpeg.decode(boxX + (boxSize - w) / 2, boxY + (boxSize - h) / 2, opt) == 1;
    jpeg.close();
  }
  if (!ok) Serial.println("cover: could not decode");
  free(buf);
  return ok;
}

static String formatTime(uint32_t ms) {
  const uint32_t s = ms / 1000;
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return String(buf);
}

// Where the song is right now, estimated between answers from Spotify.
static uint32_t estimatedProgress() {
  uint32_t p = progressMs;
  if (isPlaying) p += millis() - progressAt;
  return min(p, cur.durationMs);
}

// Draws the progress bar and times; only the parts that changed, unless
// force is set.
static void drawProgress(bool force) {
  static int lastFill = -1;
  static String lastElapsed;
  static bool lastPlaying = true;

  const uint32_t p = estimatedProgress();
  const int fill = cur.durationMs ? (int)((uint64_t)p * TEXT_W / cur.durationMs) : 0;
  if (force || fill != lastFill) {
    gfx->fillRect(TEXT_X, BAR_Y, fill, BAR_H, COL_GREEN);
    gfx->fillRect(TEXT_X + fill, BAR_Y, TEXT_W - fill, BAR_H, COL_TRACK);
    lastFill = fill;
  }

  gfx->setFont();
  gfx->setTextSize(1);
  const String elapsed = formatTime(p);
  if (force || elapsed != lastElapsed) {
    gfx->fillRect(TEXT_X, TIME_Y, 42, 8, COL_BG);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(TEXT_X, TIME_Y);
    gfx->print(elapsed);
    lastElapsed = elapsed;
  }
  if (force) {
    const String total = formatTime(cur.durationMs);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(TEXT_X + TEXT_W - (int)total.length() * 6, TIME_Y);
    gfx->print(total);
  }
  if (force || isPlaying != lastPlaying) {
    gfx->fillRect(TEXT_X + 48, TIME_Y, 44, 8, COL_BG);
    if (!isPlaying) {
      gfx->setTextColor(COL_TEXT);
      gfx->setCursor(TEXT_X + 52, TIME_Y);
      gfx->print("PAUSED");
    }
    lastPlaying = isPlaying;
  }
}

// Draws the whole screen for the song in cur.
static void drawTrack() {
  screen = SCREEN_TRACK;
  gfx->fillScreen(COL_BG);
  gfx->fillRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, COL_NOART);

  // Title: up to two lines. Artist: whatever is left of three lines.
  gfx->setFont(&FreeSansBold10pt7b);
  gfx->setTextSize(1);
  String lines[3];
  int y = FIRST_BASELINE;

  const int titleLines = wrapText(cur.title, TEXT_W, 2, lines);
  gfx->setTextColor(COL_TEXT);
  for (int i = 0; i < titleLines; i++, y += LINE_H) {
    gfx->setCursor(TEXT_X, y);
    gfx->print(lines[i]);
  }

  y += 2;
  const int artistLines = wrapText(cur.artist, TEXT_W, 3 - titleLines, lines);
  gfx->setTextColor(COL_DIM);
  for (int i = 0; i < artistLines; i++, y += LINE_H) {
    gfx->setCursor(TEXT_X, y);
    gfx->print(lines[i]);
  }

  drawProgress(true);
  drawCover(cur.artUrl, ART_X, ART_Y, ART_SIZE);   // last: it is the slow part
}

// -------------------------------- Spotify ----------------------------------

// Trades the long-lived refresh token for a one-hour access token.
static bool refreshAccessToken() {
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) return false;
  http.setAuthorization(SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const int code = http.POST(String("grant_type=refresh_token&refresh_token=") +
                             SPOTIFY_REFRESH_TOKEN);
  const String body = http.getString();
  http.end();
  if (code != 200) {
    Serial.printf("token: http %d %s\n", code, body.c_str());
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  const char *token = doc["access_token"];
  if (!token) return false;
  accessToken = token;
  const uint32_t lifeSec = doc["expires_in"] | 3600;
  tokenGoodUntil = millis() + (lifeSec > 120 ? lifeSec - 60 : lifeSec) * 1000UL;
  Serial.println("token: ok");
  return true;
}

// Picks the cover closest to `target` pixels wide from Spotify's list.
static String pickImage(JsonArrayConst images, int target = 300) {
  String best;
  int bestDiff = INT32_MAX;
  for (JsonObjectConst img : images) {
    const int w = img["width"] | 300;
    const int diff = abs(w - target);
    if (diff < bestDiff && img["url"].is<const char *>()) {
      bestDiff = diff;
      best = img["url"].as<const char *>();
    }
  }
  return best;
}

// One connection to Spotify's API, kept open and shared by the regular
// "what is playing" question and the playback commands.
static NetworkClientSecure apiClient;
static HTTPClient apiHttp;

static void apiSetup() {
  static bool ready = false;
  if (ready) return;
  apiClient.setInsecure();
  apiHttp.setReuse(true);
  apiHttp.setTimeout(8000);
  ready = true;
}

enum PollResult { POLL_TRACK, POLL_NOTHING, POLL_ERROR };

// Asks Spotify what is playing. On POLL_TRACK, fills t, playing and position.
static PollResult pollSpotify(Track &t, bool &playing, uint32_t &position) {
  NetworkClientSecure &client = apiClient;
  HTTPClient &http = apiHttp;
  apiSetup();

  if (!http.begin(client,
                  "https://api.spotify.com/v1/me/player/currently-playing?additional_types=episode")) {
    return POLL_ERROR;
  }
  http.addHeader("Authorization", "Bearer " + accessToken);
  const int code = http.GET();
  if (code == 204) {                      // nothing playing
    http.end();
    return POLL_NOTHING;
  }
  if (code != 200) {
    Serial.printf("poll: http %d\n", code);
    if (code == 401) tokenGoodUntil = millis();   // token rejected: renew it
    http.end();
    return POLL_ERROR;
  }
  const String body = http.getString();
  http.end();

  // Keep only the fields used below; the full answer is much bigger.
  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["context"]["uri"] = true;        // the playlist or album being played
  JsonObject item = filter["item"].to<JsonObject>();
  item["id"] = true;
  item["name"] = true;
  item["duration_ms"] = true;
  item["artists"][0]["name"] = true;
  item["album"]["images"][0]["url"] = true;
  item["album"]["images"][0]["width"] = true;
  item["show"]["name"] = true;            // podcasts
  item["images"][0]["url"] = true;
  item["images"][0]["width"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    Serial.println("poll: could not read Spotify's answer");
    return POLL_ERROR;
  }
  JsonObjectConst it = doc["item"];
  if (it.isNull()) return POLL_NOTHING;   // e.g. an advert

  t.id = it["id"] | "";
  t.title = toPlainText(it["name"] | "");
  t.durationMs = it["duration_ms"] | 0;

  String artist;
  for (JsonObjectConst a : it["artists"].as<JsonArrayConst>()) {
    if (artist.length()) artist += ", ";
    artist += a["name"] | "";
  }
  if (artist.length() == 0) artist = it["show"]["name"] | "";
  t.artist = toPlainText(artist.c_str());

  t.artUrl = pickImage(it["album"]["images"]);
  if (t.artUrl.length() == 0) t.artUrl = pickImage(it["images"]);

  curContext = doc["context"]["uri"] | "";
  playing = doc["is_playing"] | false;
  position = doc["progress_ms"] | 0;
  return POLL_TRACK;
}

// ------------------------------- controls ----------------------------------
// Touch the screen to control playback:
//   left third = previous song, right third = next song, middle = pause/play.
// Hold your finger for about half a second; a very quick tap can be missed
// while the board is busy talking to Spotify.

enum Command { CMD_PREVIOUS, CMD_TOGGLE, CMD_NEXT };

static uint32_t forcePollAt = 0;          // ask Spotify again at this time (0 = no)
static uint32_t labelUntil = 0;           // clear the status word at this time (0 = no)

// Shows a short word between the two times under the progress bar.
static void showLabel(const char *text, uint16_t colour) {
  gfx->setFont();
  gfx->setTextSize(1);
  gfx->fillRect(TEXT_X + 48, TIME_Y, 44, 8, COL_BG);
  gfx->setTextColor(colour);
  gfx->setCursor(TEXT_X + 52, TIME_Y);
  gfx->print(text);
}

// Sends one playback command. Returns the HTTP status (2xx = done).
static int sendCommand(Command c) {
  NetworkClientSecure &client = apiClient;
  HTTPClient &http = apiHttp;
  apiSetup();

  const char *path = c == CMD_NEXT ? "next" : c == CMD_PREVIOUS ? "previous"
                   : isPlaying ? "pause" : "play";
  if (!http.begin(client, String("https://api.spotify.com/v1/me/player/") + path)) return -1;
  http.addHeader("Authorization", "Bearer " + accessToken);
  http.addHeader("Content-Length", "0");
  const int code = http.sendRequest(c == CMD_TOGGLE ? "PUT" : "POST");
  if (code < 200 || code >= 300) {
    Serial.printf("command %s: http %d %s\n", path, code, http.getString().c_str());
  } else {
    Serial.printf("command %s: ok\n", path);
  }
  http.end();
  return code;
}

static void runCommand(Command c) {
  showLabel(c == CMD_NEXT ? "NEXT" : c == CMD_PREVIOUS ? "PREV" : isPlaying ? "PAUSE" : "PLAY",
            COL_GREEN);
  const int code = sendCommand(c);
  if (code >= 200 && code < 300) {
    if (c == CMD_TOGGLE) {
      progressMs = estimatedProgress();
      progressAt = millis();
      isPlaying = !isPlaying;
    }
    drawProgress(true);
    forcePollAt = millis() + 700;         // give Spotify a moment, then refresh
  } else {
    // 401/403: this login may not control playback (run the login script
    // again), or the account is not Premium.
    showLabel(code == 401 || code == 403 ? "DENIED" : "FAILED", rgb565(255, 80, 80));
    labelUntil = millis() + 2500;
  }
}

// ------------------------------- "up next" ---------------------------------
// Hold a finger on the middle of the song screen to see the songs coming up.
// On that screen: tap a song to play it; the arrow on the right edge shows
// more songs; the arrow on the left edge (or the header) goes back.

#define QUEUE_MAX       20
#define QUEUE_ROWS      4
#define QUEUE_ROW_H     36
#define QUEUE_FIRST_Y   28
#define QUEUE_THUMB     32
#define QUEUE_ROW_X     30      // song rows sit between the two edge arrows
#define QUEUE_ROW_W     (SCREEN_W - 2 * QUEUE_ROW_X)
#define QUEUE_EDGE      40      // touches this close to a side edge are arrows
#define QUEUE_IDLE_MS   20000   // go back to the song after this long untouched
#define LONG_PRESS_MS   600

static const uint16_t COL_ROW = rgb565(24, 24, 24);

struct QueueItem {
  String title, artist, thumbUrl, uri;
};
static QueueItem queueItems[QUEUE_MAX];
static int queueCount = 0;
static int queuePage = 0;
static uint32_t queueTouchedAt = 0;
static uint32_t lastPoll = 0;             // when Spotify was last asked (0 = ask now)

// Touches are noted as numbered events, so one that starts while the main
// code is busy (for example loading covers) is still acted on afterwards.
static volatile uint32_t touchSeq = 0;        // goes up by one for every new touch
static volatile uint32_t touchReleasedSeq = 0; // the last touch that has been lifted
static volatile uint32_t touchDownAt = 0;     // when the latest touch began / ended
static volatile uint32_t touchUpAt = 0;
static volatile int16_t touchX = 0;           // where the latest touch began
static volatile int16_t touchY = 0;

// Reads the touch chip (at most every 20 ms) and notes new touches and
// releases. Called from the main loop and from inside the slow downloads.
static void touchSample() {
  extern bool g_touch_int_flag;
  static bool down = false;
  static uint32_t lastSeen = 0;
  static uint32_t lastRead = 0;

  const uint32_t now = millis();
  if (now - lastRead < 20) return;
  lastRead = now;

  // The driver only reads the chip when this flag is set.
  g_touch_int_flag = true;
  touch_data_t touch;
  bsp_touch_read();
  if (bsp_touch_get_coordinates(&touch) && touch.touch_num > 0) {
    if (!down) {
      down = true;
      touchX = touch.coords[0].x;
      touchY = touch.coords[0].y;
      touchDownAt = now;
      touchSeq = touchSeq + 1;
    }
    lastSeen = now;
  } else if (down && now - lastSeen > 120) {
    down = false;
    touchUpAt = lastSeen;
    touchReleasedSeq = touchSeq;
  }
}

// Lets the JSON reader pull a web answer's body straight off the connection,
// whichever of the two ways the server sends it: with a known length, or in
// "chunks" that each start with their own size.
class BodyStream : public Stream {
 public:
  BodyStream(NetworkClient &client, int contentLength)
      : _client(client), _left(contentLength), _chunked(contentLength < 0) {
    setTimeout(8000);
  }
  int available() override { return _done ? 0 : _client.available(); }
  int peek() override { return -1; }
  size_t write(uint8_t) override { return 0; }
  int read() override {
    if (_done) return -1;
    if (_chunked && _left <= 0 && !nextChunk()) return -1;
    if (!_client.available()) {
      if (!_client.connected()) _done = true;
      return -1;
    }
    const int b = _client.read();
    if (b < 0) return -1;
    if (--_left == 0 && !_chunked) _done = true;
    return b;
  }

 private:
  // Reads the next chunk's size line. False if it has not fully arrived yet
  // or the body is finished.
  bool nextChunk() {
    while (_client.available()) {
      const char ch = (char)_client.read();
      if (ch != '\n') {
        _line += ch;
        continue;
      }
      String line = _line;
      _line = "";
      line.trim();
      if (line.length() == 0) continue;     // the blank line ending the previous chunk
      const long size = strtol(line.c_str(), nullptr, 16);
      if (size <= 0) {
        _done = true;
        return false;
      }
      _left = size;
      return true;
    }
    if (!_client.connected()) _done = true;
    return false;
  }

  NetworkClient &_client;
  long _left;
  bool _chunked;
  bool _done = false;
  String _line;
};

// Asks Spotify for the upcoming songs. Returns false if that failed.
static bool fetchQueue() {
  // The answer is large, so it is read piece by piece straight from the
  // connection instead of being held in memory whole.
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/queue")) return false;
  http.addHeader("Authorization", "Bearer " + accessToken);
  const int code = http.GET();
  Serial.printf("queue: http %d, %d bytes, free memory %u\n", code, http.getSize(),
                (unsigned)ESP.getFreeHeap());
  if (code != 200) {
    Serial.println(http.getString());
    http.end();
    return false;
  }
  BodyStream body(client, http.getSize());

  JsonDocument filter;
  JsonObject item = filter["queue"][0].to<JsonObject>();
  item["name"] = true;
  item["uri"] = true;
  item["artists"][0]["name"] = true;
  item["album"]["images"][0]["url"] = true;
  item["album"]["images"][0]["width"] = true;
  item["show"]["name"] = true;            // podcasts
  item["images"][0]["url"] = true;
  item["images"][0]["width"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("queue: could not read Spotify's answer (%s)\n", err.c_str());
    return false;
  }

  queueCount = 0;
  for (JsonObjectConst it : doc["queue"].as<JsonArrayConst>()) {
    if (queueCount >= QUEUE_MAX) break;
    QueueItem &q = queueItems[queueCount++];
    q.title = toPlainText(it["name"] | "");
    q.uri = it["uri"] | "";
    String artist;
    for (JsonObjectConst a : it["artists"].as<JsonArrayConst>()) {
      if (artist.length()) artist += ", ";
      artist += a["name"] | "";
    }
    if (artist.length() == 0) artist = it["show"]["name"] | "";
    q.artist = toPlainText(artist.c_str());
    q.thumbUrl = pickImage(it["album"]["images"], 64);
    if (q.thumbUrl.length() == 0) q.thumbUrl = pickImage(it["images"], 64);
  }
  Serial.printf("queue: %d songs, free memory %u\n", queueCount, (unsigned)ESP.getFreeHeap());
  return true;
}

// Draws one page of the list: text first, then the small covers.
static void drawQueuePage() {
  screen = SCREEN_QUEUE;
  gfx->fillScreen(COL_BG);

  // Header: green tick mark, title, a hint, and one dot per page.
  gfx->fillRoundRect(8, 5, 4, 17, 2, COL_GREEN);
  gfx->setFont(&FreeSansBold10pt7b);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_TEXT);
  gfx->setCursor(19, 20);
  gfx->print("Up next");

  gfx->setFont();
  gfx->setTextColor(COL_TRACK);
  gfx->setCursor(112, 10);
  gfx->print("tap a song to play");

  // Arrows at the edges: left = back, right = more songs (if there are any).
  gfx->fillTriangle(20, 84, 20, 112, 7, 98, COL_DIM);
  if ((queuePage + 1) * QUEUE_ROWS < queueCount) {
    gfx->fillTriangle(SCREEN_W - 20, 84, SCREEN_W - 20, 112, SCREEN_W - 7, 98, COL_DIM);
  }

  const int pages = (queueCount + QUEUE_ROWS - 1) / QUEUE_ROWS;
  for (int p = 0; p < pages; p++) {
    gfx->fillCircle(SCREEN_W - 12 - (pages - 1 - p) * 10, 13, 3,
                    p == queuePage ? COL_GREEN : COL_TRACK);
  }

  if (queueCount == 0) {
    gfx->setFont(&FreeSansBold10pt7b);
    gfx->setTextColor(COL_DIM);
    const String none = "Nothing queued";
    gfx->setCursor((SCREEN_W - (int)textWidth(none)) / 2, 100);
    gfx->print(none);
    return;
  }

  const int first = queuePage * QUEUE_ROWS;
  for (int i = 0; i < QUEUE_ROWS && first + i < queueCount; i++) {
    const QueueItem &q = queueItems[first + i];
    const int y = QUEUE_FIRST_Y + i * QUEUE_ROW_H;
    gfx->fillRoundRect(QUEUE_ROW_X, y, QUEUE_ROW_W, QUEUE_ROW_H - 2, 6, COL_ROW);
    gfx->fillRect(QUEUE_ROW_X + 2, y + 1, QUEUE_THUMB, QUEUE_THUMB, COL_NOART);

    const int textX = QUEUE_ROW_X + 42;
    const int textW = QUEUE_ROW_W - 42 - 8;
    gfx->setFont(&FreeSansBold10pt7b);
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(textX, y + 16);
    gfx->print(fitWithDots(q.title, textW));

    gfx->setFont();
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(textX, y + 22);
    gfx->print(q.artist.substring(0, textW / 6));
  }

  // Covers last, since they are the slow part. A new touch stops them so the
  // screen reacts straight away.
  const uint32_t seqAtStart = touchSeq;
  for (int i = 0; i < QUEUE_ROWS && first + i < queueCount; i++) {
    touchSample();
    if (touchSeq != seqAtStart) break;
    drawCover(queueItems[first + i].thumbUrl, QUEUE_ROW_X + 2,
              QUEUE_FIRST_Y + i * QUEUE_ROW_H + 1, QUEUE_THUMB);
  }
}

static void openQueue() {
  showLabel("QUEUE", COL_GREEN);
  if (!fetchQueue()) {
    showLabel("FAILED", rgb565(255, 80, 80));
    labelUntil = millis() + 2500;
    return;
  }
  queuePage = 0;
  drawQueuePage();
  queueTouchedAt = millis();
}

// Back to the song screen (it is redrawn by the next answer from Spotify).
static void closeQueue() {
  screen = SCREEN_NONE;
  shownMessage = "";
  lastPoll = 0;
}

// One request to Spotify's player. Returns the HTTP status (2xx = done).
static int playerRequest(const char *method, const char *path, const String &body = "") {
  apiSetup();
  if (!apiHttp.begin(apiClient, String("https://api.spotify.com/v1/me/player/") + path)) return -1;
  apiHttp.addHeader("Authorization", "Bearer " + accessToken);
  int code;
  if (body.length()) {
    apiHttp.addHeader("Content-Type", "application/json");
    code = apiHttp.sendRequest(method, body);
  } else {
    apiHttp.addHeader("Content-Length", "0");
    code = apiHttp.sendRequest(method);
  }
  if (code < 200 || code >= 300) {
    Serial.printf("%s %s: http %d %s\n", method, path, code, apiHttp.getString().c_str());
  }
  apiHttp.end();
  return code;
}

// Plays song number `index` of the list. Spotify has no "jump to this queued
// song" command, so:
//   1. if the music comes from a playlist or album, ask it to play that
//      playlist/album starting at the chosen song;
//   2. otherwise, or if that is refused (e.g. the song was added to the queue
//      by hand), skip forward song by song until the chosen one is reached.
static void playFromQueue(int index) {
  const QueueItem &q = queueItems[index];
  const int row = index - queuePage * QUEUE_ROWS;
  const int y = QUEUE_FIRST_Y + row * QUEUE_ROW_H;
  gfx->drawRoundRect(QUEUE_ROW_X, y, QUEUE_ROW_W, QUEUE_ROW_H - 2, 6, COL_GREEN);
  gfx->drawRoundRect(QUEUE_ROW_X + 1, y + 1, QUEUE_ROW_W - 2, QUEUE_ROW_H - 4, 5, COL_GREEN);
  Serial.printf("queue: play #%d %s\n", index + 1, q.title.c_str());

  bool ok = false;
  if (curContext.length() && q.uri.length()) {
    const String body = "{\"context_uri\":\"" + curContext + "\",\"offset\":{\"uri\":\"" +
                        q.uri + "\"}}";
    const int code = playerRequest("PUT", "play", body);
    ok = code >= 200 && code < 300;
  }
  if (!ok) {
    ok = true;
    for (int i = 0; i <= index && ok; i++) {
      const int code = playerRequest("POST", "next");
      ok = code >= 200 && code < 300;
      delay(120);
    }
  }

  if (ok) {
    forcePollAt = 0;
    delay(500);                           // give Spotify a moment to switch
    closeQueue();
  } else {
    gfx->drawRoundRect(QUEUE_ROW_X, y, QUEUE_ROW_W, QUEUE_ROW_H - 2, 6, rgb565(255, 80, 80));
    gfx->drawRoundRect(QUEUE_ROW_X + 1, y + 1, QUEUE_ROW_W - 2, QUEUE_ROW_H - 4, 5,
                       rgb565(255, 80, 80));
  }
}

// A touch on the "up next" screen: the edge arrows turn pages or go back, a
// song row plays that song, the header goes back.
static void queueTap(int x, int y) {
  queueTouchedAt = millis();
  if (x >= SCREEN_W - QUEUE_EDGE) {
    if ((queuePage + 1) * QUEUE_ROWS < queueCount) {
      queuePage++;
      drawQueuePage();
    }
  } else if (x < QUEUE_EDGE) {
    if (queuePage > 0) {
      queuePage--;
      drawQueuePage();
    } else {
      closeQueue();
    }
  } else if (y < QUEUE_FIRST_Y) {
    closeQueue();
  } else {
    const int index = queuePage * QUEUE_ROWS + (y - QUEUE_FIRST_Y) / QUEUE_ROW_H;
    if (index < queueCount) playFromQueue(index);
  }
  queueTouchedAt = millis();
}

static uint32_t touchDotUntil = 0;        // erase the "touch felt" dot at this time

static void handleTouch() {
  static uint32_t seenSeq = 0;            // the last touch already dealt with
  static bool waitingMiddle = false;      // a middle touch: tap or hold?

  touchSample();
  const uint32_t seq = touchSeq;
  if (seq != seenSeq) {
    // A new touch (it may already have been lifted again).
    seenSeq = seq;
    waitingMiddle = false;
    const int x = touchX;
    const int y = touchY;
    const int zone = x < SCREEN_W / 3 ? 0 : x >= 2 * SCREEN_W / 3 ? 2 : 1;
    Serial.printf("touch x=%d y=%d\n", x, y);
    // A green dot in the top right corner shows at once that the touch was
    // felt, even if Spotify then takes a moment to react.
    gfx->fillCircle(SCREEN_W - 6, 5, 3, COL_GREEN);
    touchDotUntil = millis() + 600;
    if (screen == SCREEN_QUEUE) {
      queueTap(x, y);
    } else if (screen == SCREEN_TRACK) {
      if (zone == 1) {
        waitingMiddle = true;             // short = pause/play, long = "up next"
      } else {
        runCommand(zone == 0 ? CMD_PREVIOUS : CMD_NEXT);
      }
    }
  }

  if (waitingMiddle && screen != SCREEN_TRACK) waitingMiddle = false;
  if (waitingMiddle) {
    if (touchReleasedSeq == seenSeq) {
      waitingMiddle = false;
      if (touchUpAt - touchDownAt >= LONG_PRESS_MS) {
        openQueue();
      } else {
        runCommand(CMD_TOGGLE);
      }
    } else if (millis() - touchDownAt >= LONG_PRESS_MS) {
      waitingMiddle = false;
      openQueue();
    }
  }

  const uint32_t now = millis();

  if (touchDotUntil && (int32_t)(now - touchDotUntil) >= 0) {
    touchDotUntil = 0;
    gfx->fillCircle(SCREEN_W - 6, 5, 3, COL_BG);
  }

  if (screen == SCREEN_QUEUE && millis() - queueTouchedAt > QUEUE_IDLE_MS) closeQueue();

  if (labelUntil && (int32_t)(now - labelUntil) >= 0) {
    labelUntil = 0;
    if (screen == SCREEN_TRACK) drawProgress(true);
  }
}

// ---------------------------------- main -----------------------------------

static bool haveSecrets() {
  return strlen(WIFI_SSID_1) && strlen(SPOTIFY_CLIENT_ID) &&
         strlen(SPOTIFY_CLIENT_SECRET) && strlen(SPOTIFY_REFRESH_TOKEN);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("Spotify display starting");

  if (!board_lcd_begin()) {
    Serial.println("gfx->begin() failed");
  }
  gfx->setRotation(SCREEN_ROTATION);
  gfx->setTextWrap(false);

  if (!haveSecrets()) {
    showMessage("Setup needed", "On the PC run: python tools\\spotify_login.py");
    while (true) delay(1000);
  }

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  bsp_touch_init(&Wire, TOUCH_RST, TOUCH_INT,
                 gfx->getRotation(), gfx->width(), gfx->height());

  WiFi.mode(WIFI_STA);
  showMessage("Connecting", "Joining WiFi...");
}

// Tries one network for up to 12 seconds.
static bool joinWiFi(const char *ssid, const char *pass) {
  if (strlen(ssid) == 0) return false;
  Serial.print("WiFi: trying ");
  Serial.println(ssid);
  WiFi.disconnect();
  WiFi.begin(ssid, pass);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(100);
  Serial.print("WiFi: status ");
  Serial.println((int)WiFi.status());
  return WiFi.status() == WL_CONNECTED;
}

// Prints every network the board can hear. passive = just listen for
// beacons; otherwise also ask networks to announce themselves.
static int scanAndPrint(bool passive) {
  const int n = WiFi.scanNetworks(false, true, passive, passive ? 600 : 300);
  Serial.print(passive ? "WiFi: passive scan found " : "WiFi: active scan found ");
  Serial.println(n);
  for (int i = 0; i < n; i++) {
    Serial.print("  '");
    Serial.print(WiFi.SSID(i));
    Serial.print("'  ch ");
    Serial.print(WiFi.channel(i));
    Serial.print("  ");
    Serial.print(WiFi.RSSI(i));
    Serial.println(" dBm");
  }
  WiFi.scanDelete();
  return n;
}

// Tries each saved network in turn. On failure, lists what the board can see
// on Serial to help find out why.
static bool connectWiFi() {
  if (joinWiFi(WIFI_SSID_1, WIFI_PASS_1) || joinWiFi(WIFI_SSID_2, WIFI_PASS_2)) return true;
  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_STA);
  delay(200);
  scanAndPrint(false);
  scanAndPrint(true);
  return false;
}

void loop() {
  static uint32_t lastBar = 0;
  static int failures = 0;

  if (WiFi.status() != WL_CONNECTED) {
    showMessage("Connecting", "Joining WiFi...");
    if (!connectWiFi()) {
      showMessage("No WiFi", "Looking for the network...");
      delay(3000);
      return;
    }
    Serial.print("WiFi: ");
    Serial.println(WiFi.SSID());
    lastPoll = 0;
  }

  const uint32_t now = millis();

  if (accessToken.length() == 0 || (int32_t)(now - tokenGoodUntil) >= 0) {
    if (screen != SCREEN_TRACK) showMessage("Connecting", "Signing in to Spotify...");
    if (!refreshAccessToken()) {
      showMessage("Spotify sign-in failed", "Run tools\\spotify_login.py again");
      delay(10000);
      return;
    }
    lastPoll = 0;
  }

  // Ask sooner when the song should just have ended.
  const bool songOver = screen == SCREEN_TRACK && isPlaying && cur.durationMs &&
                        estimatedProgress() >= cur.durationMs && now - lastPoll > 800;
  const uint32_t interval = (screen == SCREEN_TRACK && isPlaying) ? POLL_PLAYING_MS : POLL_IDLE_MS;

  // Health line every 10 seconds: WiFi signal, free memory, touches seen.
  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= 10000) {
    lastStatus = millis();
    Serial.printf("status: wifi %d dBm, free memory %u (lowest %u), touches %u\n",
                  (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(), (unsigned)touchSeq);
  }

  handleTouch();
  const bool forced = forcePollAt && (int32_t)(millis() - forcePollAt) >= 0;
  if (forced) forcePollAt = 0;

  // While the "up next" list is showing, leave the screen alone.
  if (screen == SCREEN_QUEUE) {
    delay(20);
    return;
  }

  if (lastPoll == 0 || now - lastPoll >= interval || songOver || forced) {
    Track t;
    bool playing = false;
    uint32_t position = 0;
    const PollResult r = pollSpotify(t, playing, position);
    lastPoll = millis();
    if (lastPoll == 0) lastPoll = 1;

    if (r == POLL_TRACK) {
      failures = 0;
      isPlaying = playing;
      progressMs = position;
      progressAt = millis();
      if (screen != SCREEN_TRACK || t.id != cur.id || t.title != cur.title) {
        cur = t;
        Serial.printf("now: %s - %s\n", cur.artist.c_str(), cur.title.c_str());
        drawTrack();
      } else {
        drawProgress(false);
      }
    } else if (r == POLL_NOTHING) {
      failures = 0;
      showMessage("Nothing playing", "Play something on Spotify");
    } else if (++failures >= 4) {
      showMessage("Can't reach Spotify", "Trying again...");
    }
  }

  if (screen == SCREEN_TRACK && millis() - lastBar >= 250) {
    lastBar = millis();
    drawProgress(false);
  }

  delay(20);
}
