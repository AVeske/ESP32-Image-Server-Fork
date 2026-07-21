// WT32-SC01 (V3.2) framer — a 4-slot, touch-navigable image kiosk.
//
// Holds 4 full-screen 320x480 RGB565 images in flash. The displayed slot is
// switched with a simple HTTP GET (GET /show?slot=N), so a browser, curl or
// another unit can flip its page. Each slot has an invisible 2x2 grid of touch
// hotspots; each cell can carry an HTTP GET URL. Tapping a cell fires that GET —
// a leading "/" targets this device itself (change own page), an absolute
// http://... URL targets another unit on the network (drive the fleet).
//
// WiFi: boots into SoftAP unless STA credentials were saved, then auto-joins.
// Serial console (115200, UART0):
//   wifi <ssid>:<password>   join a network (saved, auto-reconnects on boot)
//   ip / status              print the current IP / mode
//   ap                       forget wifi + start SoftAP
//   help
//
// HTTP API:
//   GET  /                serves the framer UI
//   GET  /state           { slot, slots, filled[], hasFrame, battery }
//   POST /frame?slot=N     upload 307200 B RGB565 into slot N, store + display
//   GET  /show?slot=N      display slot N (the button target)
//   GET  /buttons          read the 16-line hotspot URL table
//   POST /buttons          write the hotspot URL table
//   GET  /set?n0=LOAD&b0=90  update the data overlay (labels + percentage bars)
//   GET  /overlay          read the current overlay values
//
// The overlay is up to 4 text labels and 4 percentage bars drawn on top of the
// displayed image and refreshed with plain GETs, so a host pushes the picture
// once and then only keeps the numbers current.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <LittleFS.h>

#include "names_discovery.h"
#include "lgfx_wt32.h"
#include "overlay.h"
#include "index_html.h"

// Native panel resolution — the frame is pushed full-screen, 1:1.
static const uint16_t FRAME_W = PANEL_W;             // 320
static const uint16_t FRAME_H = PANEL_H;             // 480
static const size_t   FRAME_BYTES = (size_t)FRAME_W * FRAME_H * 2;  // 307200

static const int   NUM_SLOTS = 4;
static const int   CELLS     = 4;            // 2x2 hotspots per slot
static const char* BUTTONS_PATH = "/buttons.txt";
static String slotPath(int n) { return "/" + String(n) + ".bin"; }

// SoftAP fallback credentials. Password must be >= 8 chars, or "" for open.
static const char* AP_SSID = "WT32Framer";
static const char* AP_PASS = "wt32framer";

static const uint32_t STA_TIMEOUT_MS = 20000;

// Battery sense: the WT32-SC01 is USB-powered and exposes no battery divider by
// default, so /state reports battery: null. If you wire a 2:1 divider to an ADC
// pin, set it here (and analogReadResolution/pinMode in setup) to enable it.
static const int BAT_ADC_PIN = -1;

// Text colours (RGB565).
static const uint16_t COL_BLACK = 0x0000;
static const uint16_t COL_GREEN = 0x07E0;
static const uint16_t COL_GREY  = 0x8410;

LGFX_WT32 tft;
WebServer server(80);
Preferences prefs;

static uint8_t* frameBuf = nullptr;  // FRAME_BYTES, allocated in PSRAM (300 KB)
static size_t  frameLen = 0;
static bool    frameOk  = false; // frameBuf currently holds a valid displayed frame
static int     curSlot  = 0;
static bool    slotFilled[NUM_SLOTS] = { false };

static String   lineBuf;
static String   pendingSsid;
static bool     staConnecting = false;
static uint32_t staDeadline = 0;
static bool     wasTouched = false;

static bool apActive() {
  auto m = WiFi.getMode();
  return m == WIFI_AP || m == WIFI_AP_STA;
}

// ---- frame display + slot storage ------------------------------------------
// Push the full-screen frame to the panel, 1:1. The page packs RGB565
// big-endian, so type the source as swap565_t and let LovyanGFX convert to the
// panel's native byte order (a plain uint16 read rotates the channels).
static void pushFrame() {
  if (!frameBuf) return;
  tft.startWrite();
  tft.pushImage(0, 0, FRAME_W, FRAME_H, (const lgfx::swap565_t*)frameBuf);
  tft.endWrite();
  overlay::draw(tft);   // labels/bars sit on top of the image
}

static void refreshFilled() {
  for (int i = 0; i < NUM_SLOTS; i++) slotFilled[i] = LittleFS.exists(slotPath(i));
}

// Write frameBuf to slot n's file.
static void saveSlot(int n) {
  File f = LittleFS.open(slotPath(n), "w");
  if (!f) { Serial.println("save: open failed"); return; }
  size_t w = f.write(frameBuf, FRAME_BYTES);
  f.close();
  if (w != FRAME_BYTES) Serial.printf("save: short write %u/%u\n", (unsigned)w, (unsigned)FRAME_BYTES);
  else { slotFilled[n] = true; Serial.printf("saved slot %d\n", n); }
}

// Load slot n into frameBuf, display it, and make it the current slot.
static bool loadSlot(int n) {
  if (n < 0 || n >= NUM_SLOTS || !frameBuf) return false;
  if (!LittleFS.exists(slotPath(n))) return false;
  File f = LittleFS.open(slotPath(n), "r");
  if (!f) return false;
  size_t r = f.read(frameBuf, FRAME_BYTES);
  f.close();
  if (r != FRAME_BYTES) return false;
  curSlot = n;
  frameOk = true;
  prefs.putInt("slot", curSlot);
  pushFrame();
  return true;
}

// ---- hotspot URL table -----------------------------------------------------
// /buttons.txt is NUM_SLOTS*CELLS newline-separated lines (slot*CELLS + cell);
// an empty line means that cell has no action. Read all CELLS urls for a slot.
static void readSlotUrls(int slot, String out[CELLS]) {
  for (int i = 0; i < CELLS; i++) out[i] = "";
  File f = LittleFS.open(BUTTONS_PATH, "r");
  if (!f) return;
  int base = slot * CELLS;
  int line = 0;
  while (f.available() && line < base + CELLS) {
    String l = f.readStringUntil('\n');
    if (line >= base) { l.trim(); out[line - base] = l; }
    line++;
  }
  f.close();
}

// Fire an outbound HTTP GET to another unit. Blocking, but only for a tap.
// NOTE: never point this at our own IP — the WebServer runs single-threaded in
// loop(), so a self-request would deadlock (we'd block here instead of serving
// it). Self actions are handled locally in doHotspot() instead.
static void fireGet(const String& url) {
  Serial.println("GET " + url);
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  if (http.begin(url)) {
    int code = http.GET();
    Serial.printf("  -> %d\n", code);
    http.end();
  } else {
    Serial.println("  begin failed");
  }
}

// Run a hotspot action. Self page-switches are done locally (no HTTP round-trip
// to ourselves); only cross-device targets go out over HTTP:
//   "2"               -> show slot 2 locally (bare number 1..NUM_SLOTS)
//   "/show?slot=1"     -> show slot 1 locally (0-based, self)
//   "http://ip/..."    -> GET another unit
static void doHotspot(String url) {
  url.trim();
  if (!url.length()) return;

  bool numeric = true;
  for (size_t i = 0; i < url.length(); i++) if (!isDigit(url[i])) { numeric = false; break; }
  if (numeric) {                                  // bare 1..NUM_SLOTS = show that slot
    int n = url.toInt();
    if (n >= 1 && n <= NUM_SLOTS) { if (!loadSlot(n - 1)) Serial.printf("slot %d empty\n", n); }
    return;
  }
  if (url.startsWith("/show?slot=")) {            // self show (0-based)
    if (!loadSlot(url.substring(11).toInt())) Serial.println("self slot empty");
    return;
  }
  if (url.startsWith("http://") || url.startsWith("https://")) {
    fireGet(url);                                 // explicit URL to another unit
    return;
  }
  // "name:slot" — resolve a fleet name to its IP (slot is 1-based, like the UI).
  int colon = url.indexOf(':');
  if (colon > 0) {
    String nm = url.substring(0, colon);
    String rest = url.substring(colon + 1); rest.trim();
    bool restNum = rest.length() > 0;
    for (size_t i = 0; i < rest.length(); i++) if (!isDigit(rest[i])) restNum = false;
    if (restNum) {
      int sl = rest.toInt();
      if (nm == disco::name()) { if (!loadSlot(sl - 1)) Serial.println("self slot empty"); return; }
      IPAddress ip;
      if (disco::lookup(nm, ip)) fireGet("http://" + ip.toString() + "/show?slot=" + String(sl - 1));
      else Serial.println("hotspot: name not found: " + nm);
      return;
    }
  }
  Serial.println("hotspot: unsupported action: " + url);
}

// ---- battery (optional) ----------------------------------------------------
static bool batteryEnabled() { return BAT_ADC_PIN >= 0; }
static int  batteryMilliVolts() { return (int)analogReadMilliVolts(BAT_ADC_PIN) * 2; }
static int  batteryPercent(int mv) {
  int pct = (mv - 3300) * 100 / (4200 - 3300);
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

// ---- on-screen status ------------------------------------------------------
static void showStatus() {
  tft.fillScreen(COL_BLACK);
  tft.setTextColor(COL_GREEN, COL_BLACK);
  tft.setTextSize(2);
  tft.setCursor(6, 6);
  tft.println(disco::name());
  tft.setTextColor(COL_GREY, COL_BLACK);
  if (WiFi.status() == WL_CONNECTED) {
    tft.println("STA " + WiFi.SSID());
    tft.setTextColor(COL_GREEN, COL_BLACK);
    tft.println(WiFi.localIP().toString());
  } else if (staConnecting) {
    tft.println("joining");
    tft.println(pendingSsid);
  } else if (apActive()) {
    tft.printf("AP %s\n", AP_SSID);
    if (AP_PASS[0]) tft.printf("pw %s\n", AP_PASS);
    tft.setTextColor(COL_GREEN, COL_BLACK);
    tft.println(WiFi.softAPIP().toString());
  } else {
    tft.println("offline");
  }
  overlay::draw(tft);
}

// Repaint whatever should currently be on the panel. Called after an overlay
// change, since the labels/bars are drawn over the image rather than into it.
static void redraw() {
  if (frameOk) pushFrame();   // pushFrame() draws the overlay itself
  else showStatus();
}

// ---- serial reporting ------------------------------------------------------
static void reportIP() {
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    Serial.printf("IP %s  http://%s/  (STA \"%s\", RSSI %d)\n",
                  ip.c_str(), ip.c_str(), WiFi.SSID().c_str(), WiFi.RSSI());
  } else if (staConnecting) {
    Serial.println("joining... (no IP from DHCP yet — try 'ip' again)");
  } else if (apActive()) {
    String ip = WiFi.softAPIP().toString();
    Serial.printf("IP %s  http://%s/  (SoftAP \"%s\")\n", ip.c_str(), ip.c_str(), AP_SSID);
  } else {
    Serial.println("not connected");
  }
}

static void printHelp() {
  Serial.println("commands:");
  Serial.println("  wifi <ssid>:<password>  - join a network (saved, auto-reconnects)");
  Serial.println("  ip                      - show current IP");
  Serial.println("  status                  - mode / ssid / ip / rssi");
  Serial.println("  ap                      - forget wifi, start SoftAP");
  Serial.println("  help");
}

// ---- WiFi mode control -----------------------------------------------------
static void startAP() {
  staConnecting = false;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("SoftAP \"%s\"  http://%s/\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  if (!frameOk) showStatus();  // keep a displayed slot on screen
}

static void startSTA(const String& ssid, const String& pass, bool save) {
  if (save) {
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
  }
  pendingSsid = ssid;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);    // rejoin automatically after a WiFi-infra swap
  WiFi.begin(ssid.c_str(), pass.c_str());
  staConnecting = true;
  staDeadline = millis() + STA_TIMEOUT_MS;
  Serial.printf("joining \"%s\"... (waiting for DHCP)\n", ssid.c_str());
  if (!frameOk) showStatus();  // keep a displayed slot on screen
}

static void pollSTA() {
  if (!staConnecting) return;
  if (WiFi.status() == WL_CONNECTED) {
    staConnecting = false;
    Serial.println("connected.");
    reportIP();
    if (frameOk) pushFrame(); else showStatus();  // don't wipe the displayed slot
  } else if ((int32_t)(millis() - staDeadline) >= 0) {
    staConnecting = false;
    Serial.println("join timed out — starting SoftAP.");
    startAP();
  }
}

// ---- serial command parsing ------------------------------------------------
static void handleSerialLine(String line) {
  line.trim();
  if (!line.length()) return;
  if (line.equalsIgnoreCase("ip"))     { reportIP();  return; }
  if (line.equalsIgnoreCase("status")) { reportIP();  return; }
  if (line.equalsIgnoreCase("help"))   { printHelp(); return; }
  if (line.equalsIgnoreCase("name"))   { Serial.println("name: " + disco::name()); return; }
  if (line.length() > 5 && line.substring(0, 5).equalsIgnoreCase("name ")) {
    String nm = line.substring(5); nm.trim();
    if (nm.length()) { disco::setName(nm); Serial.println("renamed: " + disco::name()); }
    return;
  }
  if (line.equalsIgnoreCase("ap")) {
    prefs.remove("ssid");
    prefs.remove("pass");
    Serial.println("forgot saved wifi.");
    startAP();
    return;
  }

  String creds;
  if (line.length() > 5 && line.substring(0, 5).equalsIgnoreCase("wifi ")) {
    creds = line.substring(5);
  } else if (line.indexOf(':') >= 0) {
    creds = line;
  } else {
    Serial.println("unknown command — type 'help'");
    return;
  }
  creds.trim();
  int colon = creds.indexOf(':');               // split on the FIRST colon
  if (colon < 0) { Serial.println("usage: wifi <ssid>:<password>"); return; }
  String ssid = creds.substring(0, colon); ssid.trim();
  String pass = creds.substring(colon + 1);      // password may contain colons
  if (!ssid.length()) { Serial.println("empty ssid"); return; }
  startSTA(ssid, pass, true);
}

static void pumpSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { handleSerialLine(lineBuf); lineBuf = ""; }
    else if (lineBuf.length() < 160) lineBuf += c;
  }
}

// Briefly flash the given hotspot cells, then restore the image, as tap feedback.
static void flashCells(const bool cells[CELLS]) {
  int cw = tft.width() / 2, ch = tft.height() / 2;
  for (int i = 0; i < CELLS; i++) if (cells[i]) {
    int cx = (i % 2) * cw, cy = (i / 2) * ch;
    tft.fillRect(cx, cy, cw, ch, COL_GREY);
  }
  delay(60);
  pushFrame();
}

// On a touch press, map (x,y) to a 2x2 cell and run that cell's action (if set).
// Cells that share the same endpoint flash together so they read as one button.
static void pumpTouch() {
  int32_t x, y;
  bool touched = tft.getTouch(&x, &y);
  if (touched && !wasTouched) {
    int col = (x * 2) / tft.width();   if (col > 1) col = 1; if (col < 0) col = 0;
    int row = (y * 2) / tft.height();  if (row > 1) row = 1; if (row < 0) row = 0;
    int cell = row * 2 + col;
    String urls[CELLS];
    readSlotUrls(curSlot, urls);
    String url = urls[cell];
    if (url.length()) {
      bool same[CELLS];
      for (int i = 0; i < CELLS; i++) same[i] = (urls[i].length() && urls[i] == url);
      flashCells(same);
      doHotspot(url);
    }
  }
  wasTouched = touched;
}

// ---- HTTP handlers ---------------------------------------------------------
static void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// Multipart file upload: binary-safe, streamed in chunks by the core WebServer.
static void handleFrameUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    frameLen = 0;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!frameBuf) return;                                       // no PSRAM — drop it
    size_t n = up.currentSize;
    if (frameLen + n > FRAME_BYTES) n = FRAME_BYTES - frameLen;  // clamp overflow
    memcpy(frameBuf + frameLen, up.buf, n);
    frameLen += n;
  }
}

// Runs after the upload completes; stores the frame into the target slot
// (?slot=N, default = current) and displays it.
static void handleFrameDone() {
  if (frameLen != FRAME_BYTES) {
    server.send(400, "text/plain", "expected 307200 bytes of RGB565");
    return;
  }
  int slot = server.hasArg("slot") ? server.arg("slot").toInt() : curSlot;
  if (slot < 0 || slot >= NUM_SLOTS) slot = curSlot;
  saveSlot(slot);
  curSlot = slot;
  frameOk = true;
  prefs.putInt("slot", curSlot);
  pushFrame();
  server.send(200, "text/plain", "OK slot " + String(slot));
}

// GET /frame?slot=N — stream a slot's raw 307200-byte RGB565 back, so the framer
// page can show what's currently stored on a refresh. Streamed from flash (not
// via the single frameBuf) so any slot can be read without disturbing the display.
static void handleFrameGet() {
  int slot = server.hasArg("slot") ? server.arg("slot").toInt() : curSlot;
  if (slot < 0 || slot >= NUM_SLOTS) { server.send(400, "text/plain", "slot 0..3"); return; }
  if (!LittleFS.exists(slotPath(slot))) { server.send(404, "text/plain", "slot empty"); return; }
  File f = LittleFS.open(slotPath(slot), "r");
  if (!f) { server.send(500, "text/plain", "open failed"); return; }
  server.streamFile(f, "application/octet-stream");
  f.close();
}

// GET /show?slot=N — display a stored slot. This is the hotspot GET target.
static void handleShow() {
  int slot = server.hasArg("slot") ? server.arg("slot").toInt() : -1;
  if (slot < 0 || slot >= NUM_SLOTS) { server.send(400, "text/plain", "slot 0..3"); return; }
  if (!loadSlot(slot)) { server.send(404, "text/plain", "slot empty"); return; }
  server.send(200, "text/plain", "OK slot " + String(slot));
}

static void handleButtonsGet() {
  String body;
  File f = LittleFS.open(BUTTONS_PATH, "r");
  if (f) { body = f.readString(); f.close(); }
  server.send(200, "text/plain", body);
}

static void handleButtonsPost() {
  String body = server.arg("plain");
  File f = LittleFS.open(BUTTONS_PATH, "w");
  if (!f) { server.send(500, "text/plain", "open failed"); return; }
  f.print(body);
  f.close();
  server.send(200, "text/plain", "OK");
}

// GET /state: current device state for clients / the framer UI.
static void handleState() {
  String battery = "null";
  if (batteryEnabled()) {
    int mv = batteryMilliVolts();
    battery = "{\"mv\":" + String(mv) + ",\"pct\":" + String(batteryPercent(mv)) + "}";
  }
  String filled = "[";
  for (int i = 0; i < NUM_SLOTS; i++) {
    filled += slotFilled[i] ? "true" : "false";
    if (i < NUM_SLOTS - 1) filled += ",";
  }
  filled += "]";
  String s = "{\"name\":\"" + disco::name() + "\""
           + ",\"slot\":" + String(curSlot)
           + ",\"slots\":" + String(NUM_SLOTS)
           + ",\"filled\":" + filled
           + ",\"hasFrame\":" + (frameOk ? "true" : "false")
           + ",\"battery\":" + battery
           + ",\"overlay\":" + overlay::json() + "}";
  server.send(200, "application/json", s);
}

// GET /set?l0=…&n0=…&b0=90 — update the data overlay and repaint. Every
// recognised parameter in the query is applied, so one request can set several
// values at once. Unknown keys are named in a 400 rather than ignored silently.
static void handleSet() {
  String bad;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "plain") continue;
    if (!overlay::applyParam(server.argName(i), server.arg(i))) {
      if (bad.length()) bad += ",";
      bad += server.argName(i);
    }
  }
  redraw();
  if (bad.length()) server.send(400, "text/plain", "unknown parameter(s): " + bad);
  else              server.send(200, "application/json", overlay::json());
}

// GET /overlay — the current overlay values, without changing anything.
static void handleOverlay() { server.send(200, "application/json", overlay::json()); }

static void handlePeers() { server.send(200, "application/json", disco::peersJson()); }

static void handleNameGet() { server.send(200, "text/plain", disco::name()); }

static void handleNamePost() {
  String nm = server.arg("plain"); nm.trim();
  if (!nm.length()) { server.send(400, "text/plain", "empty name"); return; }
  disco::setName(nm);
  server.send(200, "text/plain", disco::name());
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(0);
  tft.setBrightness(255);
  tft.fillScreen(COL_BLACK);

  if (batteryEnabled()) { pinMode(BAT_ADC_PIN, INPUT); analogReadResolution(12); }

  // The 300 KB frame buffer doesn't fit in internal DRAM — it lives in PSRAM.
  frameBuf = (uint8_t*)ps_malloc(FRAME_BYTES);
  if (!frameBuf) Serial.println("FATAL: no PSRAM for frame buffer — pushes will be ignored");

  prefs.begin("wifi", false);

  // Mount flash and restore the last-shown slot immediately, so the panel comes
  // straight up on its page — no status screen, no WiFi wait.
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed — slots won't persist");
  refreshFilled();
  curSlot = prefs.getInt("slot", 0);
  if (curSlot < 0 || curSlot >= NUM_SLOTS) curSlot = 0;
  if (slotFilled[curSlot]) loadSlot(curSlot);   // displays it and sets frameOk

  // Bring WiFi up before the web server (lwIP init order).
  String savedSsid = prefs.getString("ssid", "");
  if (savedSsid.length()) startSTA(savedSsid, prefs.getString("pass", ""), false);
  else startAP();

  disco::begin();   // random name (persisted) + UDP discovery, after WiFi is up

  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/set", HTTP_GET, handleSet);
  server.on("/overlay", HTTP_GET, handleOverlay);
  server.on("/peers", HTTP_GET, handlePeers);
  server.on("/name", HTTP_GET, handleNameGet);
  server.on("/name", HTTP_POST, handleNamePost);
  server.on("/show", HTTP_GET, handleShow);
  server.on("/buttons", HTTP_GET, handleButtonsGet);
  server.on("/buttons", HTTP_POST, handleButtonsPost);
  server.on("/frame", HTTP_GET, handleFrameGet);
  server.on("/frame", HTTP_POST, handleFrameDone, handleFrameUpload);
  server.begin();

  Serial.println();
  Serial.println("WT32 FRAMER ready.");
  printHelp();
}

void loop() {
  server.handleClient();
  pumpSerial();
  pollSTA();
  disco::loop();   // UDP announce + peer table upkeep
  pumpTouch();
}
