/*
  Photo frame for Waveshare ESP32-C6-Touch-LCD-1.47 (portrait, 172 x 320)

  The board runs a small web server. Open its page on a phone or PC, pick
  pictures, and they appear on the display.

  How it works:
    - The web page crops and shrinks each picture to 172 x 320 in the browser
      and uploads it as raw 16-bit pixels, so any picture the browser can open
      works (JPEG, PNG, HEIC on iPhone, ...) and the board needs no decoder.
    - Pictures are stored in the board's flash, so they survive power-off.
    - The display cycles through them. Tap the screen for the next one.

  Connecting:
    - By default the board creates its own WiFi network (AP_SSID / AP_PASS
      below). Join it and open http://192.168.4.1
    - Or fill in WIFI_SSID / WIFI_PASS to make it join your own WiFi; the
      address is then shown on the display (also http://photos.local).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <vector>
#include <algorithm>
#include "board.h"
#include "esp_lcd_touch_axs5106l.h"

// ================================ settings =================================
// Leave WIFI_SSID empty to use the board's own network instead.
#define WIFI_SSID   ""
#define WIFI_PASS   ""

// The board's own network (password must be at least 8 characters).
#define AP_SSID     "PhotoFrame"
#define AP_PASS     "photos1234"

#define MAX_PICS    45      // about 110 KB each; storage is about 5.9 MB
#define DEFAULT_SEC 5       // seconds per picture (changeable on the web page)
#define INFO_MS     6000    // how long the connection info stays up at boot
// ===========================================================================

#define IMG_W      LCD_WIDTH
#define IMG_H      LCD_HEIGHT
#define IMG_BYTES  ((size_t)IMG_W * IMG_H * 2)
#define BAND_ROWS  16

static WebServer server(80);
static Preferences prefs;

static std::vector<uint32_t> ids;   // picture numbers on disk, ascending
static int cur = -1;                // index into ids of the picture on screen
static uint32_t slideSec = DEFAULT_SEC;
static uint32_t lastChange = 0;
static bool showingInfo = true;
static uint32_t infoUntil = 0;
static bool apMode = true;

static uint16_t band[IMG_W * BAND_ROWS];

// -------------------------------- storage ----------------------------------

static String picPath(uint32_t id) {
  char buf[24];
  snprintf(buf, sizeof(buf), "/p/%05lu.raw", (unsigned long)id);
  return String(buf);
}

static void loadList() {
  ids.clear();
  File dir = LittleFS.open("/p");
  if (!dir) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const char *name = f.name();
    const char *slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    const uint32_t id = strtoul(name, nullptr, 10);
    if (id > 0 && f.size() == IMG_BYTES) ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
}

static int indexOf(uint32_t id) {
  for (size_t i = 0; i < ids.size(); i++) {
    if (ids[i] == id) return (int)i;
  }
  return -1;
}

// -------------------------------- display ----------------------------------

static void drawInfo() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextWrap(false);

  gfx->setTextColor(RGB565_GREEN);
  gfx->setTextSize(2);
  gfx->setCursor(8, 14);
  gfx->print("Photo Frame");

  int16_t y = 56;
  auto label = [&](const char *s) {
    gfx->setTextColor(0x9CF3);  // grey
    gfx->setTextSize(1);
    gfx->setCursor(8, y);
    gfx->print(s);
    y += 14;
  };
  auto value = [&](const String &s) {
    gfx->setTextColor(RGB565_WHITE);
    // Size 2 fits 13 characters across; drop to size 1 for longer text.
    const bool big = s.length() <= 13;
    gfx->setTextSize(big ? 2 : 1);
    gfx->setCursor(8, y);
    gfx->print(s);
    y += big ? 30 : 22;
  };

  if (apMode) {
    label("1. Join this WiFi:");
    value(AP_SSID);
    label("   Password:");
    value(AP_PASS);
    label("2. Open in a browser:");
    value("http://" + WiFi.softAPIP().toString());
  } else {
    label("Connected to WiFi:");
    value(WIFI_SSID);
    label("Open in a browser:");
    value("http://" + WiFi.localIP().toString());
    label("or:");
    value("http://photos.local");
  }

  y += 10;
  gfx->setTextColor(0x9CF3);
  gfx->setTextSize(1);
  gfx->setCursor(8, y);
  gfx->print(ids.empty() ? "No pictures yet." : "Starting slideshow...");
  showingInfo = true;
}

static bool drawPicture(uint32_t id) {
  File f = LittleFS.open(picPath(id), "r");
  if (!f || f.size() != IMG_BYTES) return false;
  for (int16_t y = 0; y < IMG_H; y += BAND_ROWS) {
    const int16_t rows = min((int)BAND_ROWS, IMG_H - y);
    const size_t want = (size_t)IMG_W * rows * 2;
    if (f.read((uint8_t *)band, want) != want) return false;
    gfx->draw16bitRGBBitmap(0, y, band, IMG_W, rows);
  }
  return true;
}

// Shows ids[index] (wrapping), or the info screen if there are no pictures.
static void showIndex(int index) {
  lastChange = millis();
  if (ids.empty()) {
    cur = -1;
    drawInfo();
    return;
  }
  const int n = (int)ids.size();
  cur = ((index % n) + n) % n;
  if (drawPicture(ids[cur])) {
    showingInfo = false;
  } else {
    Serial.printf("could not draw picture %lu\n", (unsigned long)ids[cur]);
  }
}

// ------------------------------- web server --------------------------------

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Photo Frame</title>
<style>
:root{color-scheme:light dark;--bg:#f4f5f7;--card:#fff;--fg:#16181d;--mut:#667085;--acc:#2f6fed;--line:#d9dde3;--bad:#c62828}
@media(prefers-color-scheme:dark){:root{--bg:#111318;--card:#1b1e25;--fg:#eceef2;--mut:#98a2b3;--acc:#6b9bff;--line:#2c313a;--bad:#ff6b6b}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,sans-serif}
main{max-width:720px;margin:0 auto;padding:20px 16px 48px}
h1{font-size:22px;margin:0 0 4px}
h2{font-size:16px;margin:0 0 10px}
p{margin:0 0 10px}
.mut{color:var(--mut);font-size:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin-top:16px}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
button,.pick{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--fg);border-radius:8px;padding:8px 14px;cursor:pointer}
.pick{background:var(--acc);border-color:var(--acc);color:#fff;font-weight:600;display:inline-block}
.pick input{display:none}
button:disabled{opacity:.5;cursor:default}
select,input[type=number]{font:inherit;padding:7px 8px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--fg)}
input[type=number]{width:80px}
#msg{min-height:22px;margin-top:10px;font-size:14px}
#msg.bad{color:var(--bad)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(104px,1fr));gap:12px}
.pic{text-align:center}
.pic canvas{width:86px;height:160px;border-radius:8px;border:2px solid var(--line);background:#000;display:block;margin:0 auto 6px}
.pic.on canvas{border-color:var(--acc)}
.pic button{padding:4px 8px;font-size:13px;margin:0 1px}
.pic .del{color:var(--bad)}
</style></head><body><main>
<h1>Photo Frame</h1>
<p class="mut">Pictures are cropped to the tall 172 &times; 320 screen and stored on the board.</p>

<div class="card">
<h2>Add pictures</h2>
<div class="row">
<label class="pick">Choose pictures<input id="files" type="file" accept="image/*" multiple></label>
<label>Fit: <select id="mode"><option value="fill">Fill the screen (crop edges)</option><option value="fit">Show whole picture (black bars)</option></select></label>
</div>
<div id="msg" role="status"></div>
</div>

<div class="card">
<h2>Slideshow</h2>
<div class="row">
<label>Seconds per picture: <input id="sec" type="number" min="0" max="3600"></label>
<button id="save">Save</button>
<span class="mut">0 = stay on one picture. Tap the screen for the next one.</span>
</div>
</div>

<div class="card">
<h2>On the board <span class="mut" id="count"></span></h2>
<div class="grid" id="grid"></div>
<p class="mut" id="empty" hidden>No pictures yet.</p>
</div>
</main>
<script>
const W=172,H=320;
const $=id=>document.getElementById(id);
function say(t,bad){const m=$('msg');m.textContent=t;m.className=bad?'bad':'';}

function loadImage(file){
  return new Promise((res,rej)=>{
    const u=URL.createObjectURL(file),im=new Image();
    im.onload=()=>{URL.revokeObjectURL(u);res(im);};
    im.onerror=()=>{URL.revokeObjectURL(u);rej(new Error('this file is not a picture the browser can open'));};
    im.src=u;
  });
}

// Crops/shrinks a picture to W x H and returns it as 16-bit (RGB565) bytes.
function toFrame(im,mode){
  const sw=im.naturalWidth,sh=im.naturalHeight;
  const scale=mode==='fit'?Math.min(W/sw,H/sh):Math.max(W/sw,H/sh);
  const dw=sw*scale,dh=sh*scale;
  // Halve in steps first: one big shrink looks jagged.
  let src=im,cw=sw,ch=sh;
  while(cw/2>dw&&ch/2>dh){
    const c=document.createElement('canvas');
    c.width=Math.round(cw/2);c.height=Math.round(ch/2);
    const x=c.getContext('2d');x.imageSmoothingQuality='high';
    x.drawImage(src,0,0,c.width,c.height);
    src=c;cw=c.width;ch=c.height;
  }
  const c=document.createElement('canvas');c.width=W;c.height=H;
  const x=c.getContext('2d');x.imageSmoothingQuality='high';
  x.fillStyle='#000';x.fillRect(0,0,W,H);
  x.drawImage(src,(W-dw)/2,(H-dh)/2,dw,dh);
  const d=x.getImageData(0,0,W,H).data,out=new Uint8Array(W*H*2);
  for(let i=0,j=0;i<d.length;i+=4,j+=2){
    const v=((d[i]&0xF8)<<8)|((d[i+1]&0xFC)<<3)|(d[i+2]>>3);
    out[j]=v&255;out[j+1]=v>>8;
  }
  return out;
}

function paint(canvas,buf){
  const b=new Uint8Array(buf),x=canvas.getContext('2d'),img=x.createImageData(W,H),d=img.data;
  for(let i=0,j=0;j+1<b.length&&i<d.length;i+=4,j+=2){
    const v=b[j]|(b[j+1]<<8);
    d[i]=(v>>8)&0xF8;d[i+1]=(v>>3)&0xFC;d[i+2]=(v<<3)&0xF8;d[i+3]=255;
  }
  x.putImageData(img,0,0);
}

async function post(url){
  const r=await fetch(url,{method:'POST'});
  if(!r.ok)throw new Error(await r.text());
}

let gen=0;
async function refresh(){
  const my=++gen;
  const j=await (await fetch('/list')).json();
  if(my!==gen)return;
  if(document.activeElement!==$('sec'))$('sec').value=j.sec;
  $('count').textContent='('+j.ids.length+' of '+j.max+')';
  $('empty').hidden=j.ids.length>0;
  const grid=$('grid');grid.textContent='';
  const todo=[];
  for(const id of j.ids){
    const div=document.createElement('div');div.className='pic'+(id===j.cur?' on':'');
    const cv=document.createElement('canvas');cv.width=W;cv.height=H;
    const show=document.createElement('button');show.textContent='Show';
    show.onclick=async()=>{try{await post('/show?id='+id);refresh();}catch(e){say(e.message,true);}};
    const del=document.createElement('button');del.textContent='Delete';del.className='del';
    del.onclick=async()=>{
      if(!confirm('Delete this picture from the board?'))return;
      try{await post('/delete?id='+id);say('Picture deleted.');refresh();}catch(e){say(e.message,true);}
    };
    div.append(cv,show,del);grid.append(div);todo.push([id,cv]);
  }
  // One at a time: the board serves a single request at once.
  for(const [id,cv] of todo){
    if(my!==gen)return;
    try{paint(cv,await (await fetch('/img?id='+id)).arrayBuffer());}catch(e){}
  }
}

$('files').onchange=async e=>{
  const files=[...e.target.files];e.target.value='';
  if(!files.length)return;
  let done=0;
  for(const f of files){
    say('Uploading '+(done+1)+' of '+files.length+'...');
    try{
      const raw=toFrame(await loadImage(f),$('mode').value);
      const fd=new FormData();fd.append('f',new Blob([raw]),'p.raw');
      const r=await fetch('/upload',{method:'POST',body:fd});
      if(!r.ok)throw new Error(await r.text());
      done++;
    }catch(err){
      say('Stopped at "'+f.name+'": '+err.message+' ('+done+' of '+files.length+' uploaded)',true);
      refresh();return;
    }
  }
  say(done+(done===1?' picture':' pictures')+' uploaded.');
  refresh();
};

$('save').onclick=async()=>{
  try{await post('/set?sec='+encodeURIComponent($('sec').value));say('Slideshow time saved.');}
  catch(e){say(e.message,true);}
};

refresh().catch(e=>say('Could not reach the board: '+e.message,true));
</script></body></html>
)HTML";

static File upFile;
static uint32_t upId = 0;
static size_t upBytes = 0;
static bool upOk = false;
static const char *upErr = "upload failed";

static void abortUpload(const char *why) {
  if (upFile) upFile.close();
  if (upId) LittleFS.remove(picPath(upId));
  upId = 0;
  upErr = why;
}

// Called repeatedly while the picture's bytes arrive.
static void handleUploadData() {
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    upOk = false;
    upBytes = 0;
    upId = 0;
    upErr = "upload failed";
    if (ids.size() >= MAX_PICS) {
      upErr = "the board is full - delete a picture first";
      return;
    }
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < IMG_BYTES + 16384) {
      upErr = "no storage space left - delete a picture first";
      return;
    }
    upId = ids.empty() ? 1 : ids.back() + 1;
    upFile = LittleFS.open(picPath(upId), "w");
    if (!upFile) abortUpload("could not create the file on the board");

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!upFile) return;
    upBytes += up.currentSize;
    if (upBytes > IMG_BYTES) {
      abortUpload("picture data is the wrong size");
    } else if (upFile.write(up.buf, up.currentSize) != up.currentSize) {
      abortUpload("writing to storage failed");
    }

  } else if (up.status == UPLOAD_FILE_END) {
    if (!upFile) return;
    upFile.close();
    if (upBytes == IMG_BYTES) {
      upOk = true;
    } else {
      abortUpload("picture data is the wrong size");
    }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    abortUpload("upload was interrupted");
  }
}

// Called once when the upload request has finished.
static void handleUploadDone() {
  if (!upOk) {
    server.send(400, "text/plain", upErr);
    return;
  }
  server.send(200, "text/plain", "ok");
  const uint32_t id = upId;
  upId = 0;
  loadList();
  showIndex(indexOf(id));   // show the new picture straight away
}

// Reads ?id=N and checks that picture exists. Sends the error itself.
static bool argId(uint32_t &id) {
  id = strtoul(server.arg("id").c_str(), nullptr, 10);
  if (id == 0 || indexOf(id) < 0) {
    server.send(404, "text/plain", "no such picture");
    return false;
  }
  return true;
}

static void handleList() {
  String out = "{\"ids\":[";
  for (size_t i = 0; i < ids.size(); i++) {
    if (i) out += ',';
    out += String(ids[i]);
  }
  out += "],\"sec\":" + String(slideSec);
  out += ",\"max\":" + String(MAX_PICS);
  out += ",\"cur\":" + String(cur >= 0 && !showingInfo ? ids[cur] : 0);
  out += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

static void handleImg() {
  uint32_t id;
  if (!argId(id)) return;
  File f = LittleFS.open(picPath(id), "r");
  if (!f) {
    server.send(404, "text/plain", "no such picture");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, "application/octet-stream");
  f.close();
}

static void handleShow() {
  uint32_t id;
  if (!argId(id)) return;
  server.send(200, "text/plain", "ok");
  showIndex(indexOf(id));
}

static void handleDelete() {
  uint32_t id;
  if (!argId(id)) return;
  const bool wasCurrent = (cur >= 0 && ids[cur] == id);
  const uint32_t curId = cur >= 0 ? ids[cur] : 0;
  if (!LittleFS.remove(picPath(id))) {
    server.send(500, "text/plain", "could not delete the picture");
    return;
  }
  server.send(200, "text/plain", "ok");
  const int oldIndex = cur;
  loadList();
  if (wasCurrent || ids.empty()) {
    showIndex(oldIndex);                 // next picture slides into this slot
  } else {
    cur = indexOf(curId);                // same picture, new position
  }
}

static void handleSet() {
  const String s = server.arg("sec");
  if (s.length() == 0) {
    server.send(400, "text/plain", "enter a number of seconds");
    return;
  }
  long v = s.toInt();
  if (v < 0 || v > 3600) {
    server.send(400, "text/plain", "seconds must be between 0 and 3600");
    return;
  }
  slideSec = (uint32_t)v;
  prefs.putUInt("sec", slideSec);
  lastChange = millis();
  server.send(200, "text/plain", "ok");
}

// --------------------------------- setup -----------------------------------

static void statusLine(const char *s) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(8, 150);
  gfx->print(s);
}

static void startWiFi() {
  if (strlen(WIFI_SSID) > 0) {
    statusLine("Joining WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
      apMode = false;
      Serial.print("WiFi connected, address: ");
      Serial.println(WiFi.localIP());
      return;
    }
    Serial.println("could not join WiFi, starting own network instead");
    WiFi.disconnect(true);
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  apMode = true;
  Serial.print("own network started, address: ");
  Serial.println(WiFi.softAPIP());
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("Photo frame starting");

  if (!board_lcd_begin()) {
    Serial.println("gfx->begin() failed");
  }

  // The very first start formats the storage, which can take a while.
  statusLine("Preparing storage...");
  if (!LittleFS.begin(true)) {
    Serial.println("storage failed to start");
    statusLine("Storage failed to start");
    while (true) delay(1000);
  }
  LittleFS.mkdir("/p");
  loadList();
  Serial.printf("%u pictures stored\n", (unsigned)ids.size());

  prefs.begin("frame", false);
  slideSec = prefs.getUInt("sec", DEFAULT_SEC);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  bsp_touch_init(&Wire, TOUCH_RST, TOUCH_INT,
                 gfx->getRotation(), gfx->width(), gfx->height());

  startWiFi();
  MDNS.begin("photos");

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/list", HTTP_GET, handleList);
  server.on("/img", HTTP_GET, handleImg);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.on("/show", HTTP_POST, handleShow);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/set", HTTP_POST, handleSet);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();

  drawInfo();
  infoUntil = millis() + INFO_MS;
}

void loop() {
  server.handleClient();
  const uint32_t now = millis();

  // Connection info at boot gives way to the slideshow.
  if (showingInfo && !ids.empty() && (int32_t)(now - infoUntil) >= 0) {
    showIndex(cur < 0 ? 0 : cur);
  }

  // A new tap on the screen moves to the next picture.
  static uint32_t lastTouch = 0;
  touch_data_t touch;
  bsp_touch_read();
  if (bsp_touch_get_coordinates(&touch) && touch.touch_num > 0) {
    if (now - lastTouch > 350 && !ids.empty()) {
      showIndex(showingInfo ? 0 : cur + 1);
    }
    lastTouch = now;
  }

  if (!showingInfo && slideSec > 0 && ids.size() > 1 &&
      now - lastChange >= slideSec * 1000UL) {
    showIndex(cur + 1);
  }

  delay(2);
}
