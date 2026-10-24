// AtomS3R web image server — a 4-slot image kiosk with single-button gestures.
//
// Holds 4 128x128 RGB565 images in flash. The displayed slot is switched with a
// simple HTTP GET (GET /show?slot=N). The AtomS3R has one button, so each slot
// maps three button gestures to HTTP GET actions: short click (<500 ms), long
// click (>1500 ms), and double click (two shorts within 2000 ms). A gesture URL
// of "2" shows slot 2 here; "/show?slot=1" is also local; "http://other/…" drives
// another unit.
//
// A live data overlay (up to 4 text labels + 4 percentage bars) can be drawn on
// top of the displayed image and updated with plain GETs — GET /set?n0=LOAD&b0=90
// — so a host pushes the picture once and then only refreshes the numbers.
//
// ?mid= on /frame and markerId in /state carry an optional tag id for the image
// in a slot (used by a marker-tracking host); leave them unset if you don't
// need one.
//
// Serial console (115200, USB-CDC): wifi <ssid>:<password> / ip / status / ap.

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <LittleFS.h>

#include "names_discovery.h"
#include "overlay.h"
#include "index_html.h"
#include "letter_selector.h"
#include "letter_sender.h"
#include <HTTPClient.h>

#include "server_client.h"
#include <ArduinoJson.h>
// Keep header-defined configuration local to this translation unit.
namespace {
#include "secrets.h"
}

static const uint16_t FRAME_W = 128;
static const uint16_t FRAME_H = 128;
static const size_t   FRAME_BYTES = (size_t)FRAME_W * FRAME_H * 2;  // 32768

static const int   NUM_SLOTS = 4;
static const int   GESTURES  = 3;            // short / long / double per slot
static const char* BUTTONS_PATH = "/buttons.txt";
static String slotPath(int n) { return "/" + String(n) + ".bin"; }

static const char* STATION_URL = "http://192.168.4.2:8000";

// Single-button gesture thresholds.
static const uint32_t SHORT_MAX_MS = 500;    // release before this = short click
static const uint32_t LONG_MIN_MS  = 700;    // held at least this  = long click
static const uint32_t DOUBLE_MS    = 1500;   // 2nd short within this of 1st = double

// Battery sense ADC. AtomS3R / AtomS3 read the pack through a 2:1 divider on
// GPIO 8 (the older Atom series uses GPIO 33). analogReadMilliVolts() already
// applies the eFuse calibration, so we just double it back to pack volts.
static const int      BAT_ADC_PIN = 8;
static const int      BAT_FULL_MV = 4200;  // 1S LiPo at 100%
static const int      BAT_EMPTY_MV = 3300; // treat as 0%

// SoftAP fallback credentials. Password must be >= 8 chars, or "" for open.
static const char* AP_SSID = "AtomFramer";
static const char* AP_PASS = "atomframer";

static const uint32_t STA_TIMEOUT_MS = 20000;  // give up on a join after this

WebServer server(80);
Preferences prefs;
DNSServer dnsServer;

static uint8_t frameBuf[FRAME_BYTES];
static size_t  frameLen = 0;     // bytes received so far in the current upload
static bool    frameOk  = false; // a full frame is currently stored/displayed
static bool    letterMode = false;
static int     markerId = -1;    // ArUco id of the current slot (-1 = unknown)
static int     curSlot  = 0;
static bool    slotFilled[NUM_SLOTS] = { false };

static String  lineBuf;          // serial line accumulator
static String pendingPass;
static bool staEnabled = false;
static bool triedSecretsFallback = false;
static uint32_t nextStaRetry = 0;
static const uint32_t STA_RETRY_MS = 10000;
static String  pendingSsid;      // ssid we're currently trying to join
static bool    staConnecting = false;
static uint32_t staDeadline = 0;

static bool apActive() {
  auto m = WiFi.getMode();
  return m == WIFI_AP || m == WIFI_AP_STA;
}

// ---- frame display + persistence -------------------------------------------
// Push frameBuf to the panel. The page packs RGB565 big-endian (high byte
// first); typing the source as swap565_t lets M5GFX convert from that byte
// order to the panel's native order itself (reading it as a plain uint16
// rotates the channels: red->blue, green->red, blue->green).
static void pushFrame() {
  if (letterMode) return;
  M5.Display.startWrite();
  M5.Display.pushImage(0, 0, FRAME_W, FRAME_H, (const m5gfx::swap565_t*)frameBuf);
  M5.Display.endWrite();
  overlay::draw(M5.Display);   // labels/bars sit on top of the image
}

// Called only from the main loop, never from the network worker.
static bool displayServerPng(const uint8_t* png, size_t length) {
  Serial.printf("PNG decode: bytes=%u heap=%u largest=%u\n",
                (unsigned)length, (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
  M5Canvas image(&M5.Display);
  // The existing frame is already a 128x128 big-endian RGB565 buffer.
  // Attach it instead of allocating a second 32 KiB framebuffer.
  image.setBuffer(frameBuf, FRAME_W, FRAME_H, 16);
  image.fillScreen(TFT_BLACK);
  bool decoded = image.drawPng(png, length, 0, 0);
  image.deleteSprite(); // Detaches the externally owned buffer without freeing it.
  M5.Display.releasePngMemory();
  if (!decoded) {
    Serial.printf("PNG decoder rejected image: bytes=%u heap=%u largest=%u\n",
                  (unsigned)length, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
    frameOk = false;
    return false;
  }
  letterMode = false;
  markerId = -1;
  frameOk = true;
  pushFrame();
  // Remote jobs do not overwrite any of the four locally saved slots.
  return true;
}

static String midKey(int n) { return "mid" + String(n); }

static void refreshFilled() {
  for (int i = 0; i < NUM_SLOTS; i++) slotFilled[i] = LittleFS.exists(slotPath(i));
}

// Write frameBuf (and the current marker id) to slot n.
static void saveSlot(int n) {
  File f = LittleFS.open(slotPath(n), "w");
  if (!f) { Serial.println("save: open failed"); return; }
  size_t w = f.write(frameBuf, FRAME_BYTES);
  f.close();
  prefs.putInt(midKey(n).c_str(), markerId);
  if (w != FRAME_BYTES) Serial.printf("save: short write %u/%u\n", (unsigned)w, (unsigned)FRAME_BYTES);
  else { slotFilled[n] = true; }
}

// Load slot n into frameBuf, display it, and make it the current slot.
static bool loadSlot(int n) {
  if (n < 0 || n >= NUM_SLOTS || !LittleFS.exists(slotPath(n))) return false;
  File f = LittleFS.open(slotPath(n), "r");
  if (!f) return false;
  size_t r = f.read(frameBuf, FRAME_BYTES);
  f.close();
  if (r != FRAME_BYTES) return false;
  markerId = prefs.getInt(midKey(n).c_str(), -1);
  curSlot = n;
  frameOk = true;
  prefs.putInt("slot", curSlot);
  letterMode = false;
  pushFrame();
  return true;
}

// ---- hotspot / gesture URL table -------------------------------------------
// /buttons.txt is NUM_SLOTS*GESTURES newline-separated lines (slot*GESTURES + g,
// g = 0 short / 1 long / 2 double); an empty line means that gesture has no
// action. Read all GESTURES urls for a slot.
static void readSlotUrls(int slot, String out[GESTURES]) {
  for (int i = 0; i < GESTURES; i++) out[i] = "";
  File f = LittleFS.open(BUTTONS_PATH, "r");
  if (!f) return;
  int base = slot * GESTURES;
  int line = 0;
  while (f.available() && line < base + GESTURES) {
    String l = f.readStringUntil('\n');
    if (line >= base) { l.trim(); out[line - base] = l; }
    line++;
  }
  f.close();
}

// Fire an outbound HTTP GET to another unit. Blocking, but only for a button
// press. Never aim this at our own IP — the single-threaded WebServer would
// deadlock on a self-request; self actions are handled locally in doAction().
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

// Run a gesture action. Self page-switches are done locally (no self HTTP):
//   "2"            -> show slot 2 here (bare number 1..NUM_SLOTS)
//   "/show?slot=1" -> show slot 1 here (0-based, self)
//   "http://ip/…"  -> GET another unit
static void doAction(String url) {
  url.trim();
  if (!url.length()) return;
  bool numeric = true;
  for (size_t i = 0; i < url.length(); i++) if (!isDigit(url[i])) { numeric = false; break; }
  if (numeric) {
    int n = url.toInt();
    if (n >= 1 && n <= NUM_SLOTS) { if (!loadSlot(n - 1)) Serial.printf("slot %d empty\n", n); }
    return;
  }
  if (url.startsWith("/show?slot=")) {
    if (!loadSlot(url.substring(11).toInt())) Serial.println("self slot empty");
    return;
  }
  if (url.startsWith("http://") || url.startsWith("https://")) { fireGet(url); return; }
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
      else Serial.println("action: name not found: " + nm);
      return;
    }
  }
  Serial.println("action: unsupported target: " + url);
}

// ---- battery ---------------------------------------------------------------
static int batteryMilliVolts() {
  return (int)analogReadMilliVolts(BAT_ADC_PIN) * 2;  // undo the 2:1 divider
}

static int batteryPercent(int mv) {
  int pct = (mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

// ---- on-screen status ------------------------------------------------------
static void showStatus() {
  if (letterMode) return;
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(3, 3);
  M5.Display.println(disco::name());
  M5.Display.setTextColor(0x8410, TFT_BLACK);  // dim grey
  if (WiFi.status() == WL_CONNECTED) {
    M5.Display.println("STA " + WiFi.SSID());
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.println(WiFi.localIP().toString());
  } else if (staConnecting) {
    M5.Display.println("joining");
    M5.Display.println(pendingSsid);
  } else if (apActive()) {
    M5.Display.printf("AP %s\n", AP_SSID);
    if (AP_PASS[0]) M5.Display.printf("pw %s\n", AP_PASS);
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.println(WiFi.softAPIP().toString());
  } else {
    M5.Display.println("offline");
  }
  overlay::draw(M5.Display);
}

// Repaint whatever should currently be on the panel. Called after an overlay
// change, since the labels/bars are drawn over the image rather than into it.
static void redraw() {
  if (letterMode) {
    showSelectedLetter();
  } else if (frameOk) {
    pushFrame();
  } else {
    showStatus();
  }
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
static void startAP(bool keepStation = false) {
  staConnecting = false;
  if (!keepStation) staEnabled = false;
  WiFi.mode(keepStation ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  dnsServer.stop();
  dnsServer.start(53, "*", WiFi.softAPIP());

  Serial.printf("SoftAP \"%s\"  http://%s/\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  if (!frameOk) showStatus();  // keep a restored frame on screen; status is on the button
}

static void startSTA(const String& ssid, const String& pass, bool save) {
  if (save) {
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
  }
  pendingSsid = ssid;
  pendingPass = pass;
  staEnabled = true;
  // Keep the laptop/station network available while STA provides internet.
  if (!apActive()) startAP(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);    // rejoin automatically after a WiFi-infra swap
  if (pass.length() == 0) {

    WiFi.begin(
        ssid.c_str()
    );

} else {

    WiFi.begin(
        ssid.c_str(),
        pass.c_str()
    );
}
  staConnecting = true;
  staDeadline = millis() + STA_TIMEOUT_MS;
  if (!frameOk) showStatus();  // keep a restored frame on screen; status is on the button
}

// Watch a pending STA join; report the IP once DHCP lands, or fall back to AP.
static void pollSTA() {

    if (!staEnabled) return;
    if (!staConnecting) {
        if (WiFi.status() == WL_CONNECTED) {
            nextStaRetry = 0;
            return;
        }
        if (nextStaRetry == 0) {
            nextStaRetry = millis() + STA_RETRY_MS;
            Serial.println("[WiFi] Disconnected; retrying in 10 seconds.");
        }
        if ((int32_t)(millis() - nextStaRetry) < 0) return;
        nextStaRetry = 0;
        Serial.println("[WiFi] Retrying station connection.");
        startSTA(pendingSsid, pendingPass, false);
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {

        staConnecting = false;
        // Keep the credentials that actually worked for the next boot.
        prefs.putString("ssid", pendingSsid);
        prefs.putString("pass", pendingPass);

        static bool connectionReported = false;
        if (!connectionReported) {
            Serial.printf("WiFi connected: %s | IP: %s\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
            connectionReported = true;
        }

        if (frameOk) {
            pushFrame();
        } else {
            showStatus();
        }

    } else if ((int32_t)(millis() - staDeadline) >= 0) {

        staConnecting = false;

        Serial.println();
        Serial.println("===== WIFI CONNECTION FAILED =====");

        Serial.printf(
            "SSID: %s\n",
            pendingSsid.c_str()
        );

        Serial.printf(
            "Final WiFi status: %d\n",
            WiFi.status()
        );

        Serial.println(
            "Timed out after 20 seconds."
        );

        Serial.println(
            "Starting AtomFramer AP."
        );

        Serial.println(
            "=================================="
        );

        if (!triedSecretsFallback && strlen(WIFI_SSID) > 0 &&
            (pendingSsid != WIFI_SSID || pendingPass != WIFI_PASSWORD)) {
            triedSecretsFallback = true;
            Serial.println("[WiFi] Saved credentials failed; trying secrets.h credentials.");
            startSTA(String(WIFI_SSID), String(WIFI_PASSWORD), false);
            return;
        }
        startAP(true);
        nextStaRetry = millis() + STA_RETRY_MS;
        Serial.println("[WiFi] AP available; station retry in 10 seconds.");
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

  // "wifi ssid:pass", or a bare "ssid:pass" line.
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

// ---- single-button gestures ------------------------------------------------
static uint32_t pressStart   = 0;
static bool     shortPending = false;
static uint32_t firstShortAt = 0;

// Run the current slot's action for gesture g (0 short / 1 long / 2 double).
// An unconfigured long-press falls back to showing WiFi/IP status on the LCD.
static void runGesture(int g) {
  String urls[GESTURES];
  readSlotUrls(curSlot, urls);
  String url = urls[g];
  Serial.printf("gesture %d -> \"%s\"\n", g, url.c_str());
  if (url.length()) doAction(url);
  else if (g == 1) showStatus();
}

// Short releases select A-Z; long releases enqueue the selected letter.
// All station HTTP runs in the worker so button sampling remains responsive.
static void pumpButton() {
  if (M5.BtnA.wasPressed()) {
    pressStart = millis();
    Serial.println("BtnA pressed");
  }

  if (!M5.BtnA.wasReleased()) return;

  uint32_t duration = millis() - pressStart;
  Serial.printf("BtnA released: %lu ms\n", (unsigned long)duration);

  if (duration >= LONG_MIN_MS) {
    char letter = getSelectedLetter();
    bool queued = queueStationLetter(letter);
    Serial.printf("Letter %c: %s\n", letter, queued ? "queued for station" : "send queue unavailable");
    // Long press sends the selected letter; it must not advance the alphabet.
    letterMode = true;
    showSelectedLetter();
    return;
  }

  // Esimene vajutus kuvab A; järgmised valivad järgmise tähe.
  if (letterMode) {
    nextLetter();
  }

  letterMode = true;
  showSelectedLetter();
  Serial.printf("Selected letter: %c\n", getSelectedLetter());
}

// ---- HTTP handlers ---------------------------------------------------------
static void handlePortalRedirect() {
  String url = "http://" + WiFi.softAPIP().toString() + "/";
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Location", url, true);
  server.send(302, "text/plain", "Open " + url);
}

static void handleRoot() {
  // Canonical numeric URL avoids keeping a connectivity-check hostname open.
  String host = server.hostHeader();
  String localIP = server.client().localIP().toString();
  if (apActive() && host.length() && host != localIP && host != localIP + ":80") {
    handlePortalRedirect();
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
}

// Multipart file upload: binary-safe, streamed in chunks by the core WebServer.
static void handleFrameUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    frameLen = 0;
    frameOk = false;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    size_t n = up.currentSize;
    if (frameLen + n > FRAME_BYTES) n = FRAME_BYTES - frameLen;  // clamp overflow
    memcpy(frameBuf + frameLen, up.buf, n);
    frameLen += n;
  } else if (up.status == UPLOAD_FILE_END) {
    frameOk = (frameLen == FRAME_BYTES);
  }
}

// Runs after the upload completes; stores the frame into the target slot
// (?slot=N, default = current) and displays it. ?mid=<n> records the ArUco id.
static void handleFrameDone() {
  if (!frameOk) {
    server.send(400, "text/plain", "expected 32768 bytes of RGB565");
    return;
  }
  int slot = server.hasArg("slot") ? server.arg("slot").toInt() : curSlot;
  if (slot < 0 || slot >= NUM_SLOTS) slot = curSlot;
  markerId = server.hasArg("mid") ? server.arg("mid").toInt() : -1;
  saveSlot(slot);
  curSlot = slot;
  prefs.putInt("slot", curSlot);
  pushFrame();
  server.send(200, "text/plain", "OK slot " + String(slot));
}

// GET /frame?slot=N — stream a slot's raw 32768-byte RGB565 back (read-back).
static void handleFrameGet() {
  int slot = server.hasArg("slot") ? server.arg("slot").toInt() : curSlot;
  if (slot < 0 || slot >= NUM_SLOTS) { server.send(400, "text/plain", "slot 0..3"); return; }
  if (!LittleFS.exists(slotPath(slot))) { server.send(404, "text/plain", "slot empty"); return; }
  File f = LittleFS.open(slotPath(slot), "r");
  if (!f) { server.send(500, "text/plain", "open failed"); return; }
  server.streamFile(f, "application/octet-stream");
  f.close();
}

// GET /show?slot=N — display a stored slot. This is the gesture GET target.
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

// GET /state: current device state. Keeps markerId + battery for atom-manager,
// adds the current slot and which slots are filled.
static void handleState() {
  int mv = batteryMilliVolts();
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
           + ",\"markerId\":" + String(markerId)
           + ",\"hasFrame\":" + (frameOk ? "true" : "false")
           + ",\"battery\":{\"mv\":" + String(mv)
           + ",\"pct\":" + String(batteryPercent(mv)) + "}"
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

static void handleNotFound() {
  if (apActive() && server.method() == HTTP_GET) handlePortalRedirect();
  else server.send(404, "text/plain", "Not found");
}

static void handleStationResult() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) || !doc["id"].is<int>() ||
      doc["id"].as<int>() < 0 || !doc["result"].is<const char*>()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid result payload\"}");
    return;
  }
  const char* result = doc["result"];
  if (strcmp(result, "done") && strcmp(result, "error")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Expected done or error\"}");
    return;
  }
  int state = queueStationResult(doc["id"].as<int>(), result);
  // The station retries until public Flask has actually confirmed delivery.
  if (state == 1) server.send(200, "application/json", "{\"ok\":true}");
  else if (state == -2) server.send(409, "application/json", "{\"ok\":false,\"error\":\"Conflicting result\"}");
  else server.send(503, "application/json", "{\"ok\":false,\"pending\":true}");
}

static void handleTestDisplay(){
  letterMode = false;
  showStatus();
  server.send(200, "text/plain", "Status shown on display");
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);

  initLetterSelector();

  // Battery ADC: 12-bit, 2:1 divider on GPIO 8 (see batteryMilliVolts()).
  pinMode(BAT_ADC_PIN, INPUT);
  analogReadResolution(12);

  Serial.begin(115200);
  prefs.begin("wifi", false);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.printf("[WiFi] Station disconnected: reason=%u\n",
                    info.wifi_sta_disconnected.reason);
    }
  });

  // Mount flash and restore the last-shown slot immediately, so the panel comes
  // straight up on its page — no boot animation, no status screen, no WiFi wait.
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed — slots won't persist");
  refreshFilled();
  curSlot = prefs.getInt("slot", 0);
  if (curSlot < 0 || curSlot >= NUM_SLOTS) curSlot = 0;
  if (slotFilled[curSlot]) loadSlot(curSlot);   // displays it and sets frameOk

  // Bring WiFi up FIRST — this initialises the lwIP/TCP-IP stack. Starting the
  // web server before any WiFi.mode() call asserts "Invalid mbox" in lwIP.
  String savedSsid = prefs.getString("ssid", "");

  if (savedSsid.length()) {

    startSTA(
        savedSsid,
        prefs.getString("pass", ""),
        false
    );

  } else if (strlen(WIFI_SSID) > 0) {

      startSTA(
          String(WIFI_SSID),
          String(WIFI_PASSWORD),
          true
      );

  } else {

      Serial.println(
          "No WiFi credentials available -> starting AP only"
      );

      startAP();
  }

  setServerImageHandler(displayServerPng);
  initServerClient(STATION_URL);

  disco::begin();   // random name (persisted) + UDP discovery, after WiFi is up

  server.on("/api/result", HTTP_POST, handleStationResult);
  server.on("/", HTTP_GET, handleRoot);
  // Connectivity probes must signal a captive portal, not internet access.
  server.on("/connecttest.txt", HTTP_GET, handlePortalRedirect);
  server.on("/ncsi.txt", HTTP_GET, handlePortalRedirect);
  server.on("/redirect", HTTP_GET, handlePortalRedirect);
  server.on("/generate_204", HTTP_GET, handlePortalRedirect);
  server.on("/gen_204", HTTP_GET, handlePortalRedirect);
  server.on("/hotspot-detect.html", HTTP_GET, handlePortalRedirect);
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
  // POST /frame: (responder, upload-handler) — the upload handler fires first.
  server.on("/frame", HTTP_POST, handleFrameDone, handleFrameUpload);
  server.on("/test-display", HTTP_GET, handleTestDisplay);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println();
  Serial.printf("ATOM FRAMER ready — name \"%s\".\n", disco::name().c_str());
}

void loop() {
  M5.update();
  pumpButton();
  if (apActive()) dnsServer.processNextRequest();
  server.handleClient();
  pumpSerial();
  pollSTA();
  pollServer();
  // Network transitions invalidate UDP sockets; bind again once an IP exists.
  static IPAddress discoveryIP;
  IPAddress currentIP = WiFi.status() == WL_CONNECTED ? WiFi.localIP()
                         : (apActive() ? WiFi.softAPIP() : IPAddress());
  if (currentIP != discoveryIP) {
    disco::_udp.stop();
    if ((uint32_t)currentIP != 0) disco::_udp.begin(disco::PORT);
    discoveryIP = currentIP;
  }
  if ((uint32_t)currentIP != 0) disco::loop();
  delay(1); // Yield while the network worker handles HTTP.
}
