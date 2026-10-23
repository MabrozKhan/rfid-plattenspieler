/*
  RFID turntable, firmware 2.2

  Usage
    Put a record on           link is read, no music yet
    Lower the tonearm         album starts at track 1, platter turns
    Lift the tonearm          pause, platter stops
    Lower it again            continues where it stopped
    Take the record off       pause and reset, next time it starts from the beginning
    Turn the knob             volume in steps of 5 %, also while paused
    Press the knob            mute on/off
    Next / previous buttons   skip tracks, also while paused
    Shuffle button (puck)     shuffle on/off

  Board        ESP32 Dev Module
  Libraries    MFRC522 (GithubCommunity), Adafruit SSD1306, Adafruit GFX,
               ArduinoJson 7
  Serial       115200 baud
  Credentials  secrets.h (copy secrets.example.h)

  Wiring
    RC522            SDA 5, SCK 18, MOSI 23, MISO 19, RST 22, 3.3V to 3V3
    ULN2003 (motor)  IN1 25, IN2 26, IN3 27, IN4 14, + to VIN (5 V)
    KY-040 encoder   CLK 34, DT 35, SW 15, + to 3V3 (not 5 V)
    Next button      GPIO 33 to GND
    Previous button  GPIO 32 to GND
    Shuffle button   GPIO 17 to GND
    Tonearm switch   micro switch, COM to GND, NO to GPIO 16
    SSD1306 128x32   SDA 21, SCL 4, VCC to 3V3

  Structure
    loop()         reads record, tonearm, buttons and encoder and decides what
                   should happen. Never waits for the network, so the inputs
                   always react immediately.
    spotifyTask()  sends the commands to the Spotify Web API over one kept-alive
                   HTTPS connection (saves about 1 s per command).
    motorTask()    steps the platter motor at a fixed rate.
    displayTask()  redraws the OLED about 20 times per second.

    loop() hands jobs to spotifyTask() through a small shared job list. Jobs
    that come in faster than Spotify can handle them are merged, e.g. five
    encoder detents during a running request become one "+25 %".
*/

#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>        // only for the token request
#include <Preferences.h>       // NVS flash storage
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

#include "secrets.h"           // WiFi, client ID, refresh token, device ID

// ===========================================================================
// Settings
// ===========================================================================

// --- RC522 ---
const uint8_t PIN_SS  = 5;     // labelled SDA on the RC522
const uint8_t PIN_RST = 22;

// --- 28BYJ-48 stepper via ULN2003 ---
const uint8_t PIN_MOTOR[4] = { 25, 26, 27, 14 };   // IN1 to IN4
// Full step (two coils on, more torque): 2048 steps per revolution
//   15 ms = 2 rpm, 12 ms = 2.4 rpm, 8 ms = 3.7 rpm, 4 ms = 7 rpm
// Half step (quieter, less torque): 4096 steps per revolution
const bool     MOTOR_FULL_STEP = true;
const uint32_t MOTOR_STEP_MS   = 12;
const bool     MOTOR_REVERSE   = false;

// --- KY-040 rotary encoder ---
// GPIO 34/35 are input only without internal pull-ups, the KY-040 board
// has its own.
const uint8_t PIN_ENC_CLK = 34;
const uint8_t PIN_ENC_DT  = 35;
const uint8_t PIN_ENC_SW  = 15;
const int8_t  ENC_TRANSITIONS_PER_DETENT = 4;   // usually 4 on the KY-040
const bool    ENC_REVERSE                = false;
const int8_t  VOLUME_STEP                = 5;   // percent per detent

// --- Buttons (all switch to GND, INPUT_PULLUP, pressed = LOW) ---
const uint8_t PIN_NEXT    = 33;
const uint8_t PIN_PREV    = 32;
const uint8_t PIN_SHUFFLE = 17;
const uint8_t PIN_TONEARM = 16;
// true: switch pressed means the arm is on the record. Depends on the mechanics.
const bool TONEARM_PRESSED_MEANS_DOWN = false;
const uint32_t DEBOUNCE_MS = 50;

// --- Record ---
const uint32_t POLL_INTERVAL_MS = 250;
// The record only counts as removed after this many misses in a row
// (about 1 s), single read errors are ignored.
const uint8_t  MISSES_UNTIL_GONE = 4;
// NTAG213 user memory: pages 4 to 39 = 144 bytes
const uint8_t  FIRST_PAGE    = 4;
const uint16_t USER_DATA_LEN = 144;

// --- Spotify ---
const char*    API_HOST = "api.spotify.com";
// If the stored device ID is unknown, the Echo is searched in the device list
// by this part of its name (case insensitive).
const char*    DEVICE_NAME_PART = "echo";
// Spotify closes idle connections at some point, so reconnect after this
const uint32_t CONN_IDLE_MS        = 20000;
const uint32_t RESPONSE_TIMEOUT_MS = 8000;
// Self healing: after this many failed requests in a row reconnect WiFi,
// after twice as many restart the ESP32.
const uint8_t  ERRORS_UNTIL_WIFI_RESET = 2;
// Volume may have been changed by voice, so read it again from Spotify if
// it was not touched for this long
const uint32_t VOLUME_REREAD_MS = 10000;
const uint32_t TRACK_POLL_MS    = 5000;

// Spotify sometimes accepts the play command while the Echo is asleep and
// nothing happens, so the start is checked.
const uint32_t START_CHECK_MS       = 7000;
const uint8_t  START_ROUNDS         = 3;       // automatic retries
const uint32_t START_ROUND_PAUSE_MS = 10000;
// Watchdog: restart if spotifyTask hangs for this long
const uint32_t WATCHDOG_MS          = 120000;

// --- Display ---
// GPIO 22 is used by the RC522, so SCL is on GPIO 4
const uint8_t  PIN_OLED_SDA = 21;
const uint8_t  PIN_OLED_SCL = 4;
const uint8_t  OLED_ADDRESS = 0x3C;
const uint32_t OVERLAY_MS   = 1500;   // big volume / shuffle overlay
const uint32_t NOTE_MS      = 4000;

// ===========================================================================
// Types and shared data
// ===========================================================================

enum Playback : uint8_t { PB_NONE, PB_START, PB_RESUME, PB_PAUSE };

// Job list between loop() and spotifyTask()
struct Jobs {
  Playback playback = PB_NONE;   // only the last request counts
  String   uri;                  // PB_START: which album
  uint32_t recordNo = 0;         // PB_START: which placement of a record
  int8_t   skips = 0;            // +2 = next twice, -1 = previous once
  int16_t  volumeChange = 0;     // percent, summed up
  bool     toggleMute = false;
  bool     toggleShuffle = false;
};

struct Button {
  uint8_t  pin;
  bool     pressed;      // debounced state
  bool     lastRaw;
  uint32_t lastChange;
};

Jobs              pendingJobs;
SemaphoreHandle_t jobLock;

// written by loop(), read by the other tasks
volatile bool     shouldPlay      = false;
volatile bool     motorShouldRun  = false;
// written by spotifyTask(): placement number of the record that is playing
volatile uint32_t startedRecordNo = 0;
// written by spotifyTask(): last sign of life, for the watchdog
volatile uint32_t spotifyAliveMs  = 0;

// --- loop() only ---
MFRC522  rfid(PIN_SS, PIN_RST);
bool     recordPresent = false;
bool     recordRead    = false;
uint8_t  missCount     = 0;
uint32_t recordNo      = 0;     // counts up on every placement
String   currentUri;
bool     armDown       = false;
bool     lastShouldPlay = false;
uint32_t lastPoll      = 0;

Button btnNext    = { PIN_NEXT,    false, false, 0 };
Button btnPrev    = { PIN_PREV,    false, false, 0 };
Button btnShuffle = { PIN_SHUFFLE, false, false, 0 };
Button btnArm     = { PIN_TONEARM, false, false, 0 };
Button btnEncoder = { PIN_ENC_SW,  false, false, 0 };

// --- encoder, changed in the ISR ---
volatile int32_t encCount = 0;
volatile uint8_t encState = 0;
portMUX_TYPE     encLock  = portMUX_INITIALIZER_UNLOCKED;

// --- spotifyTask() only (and once in setup) ---
Preferences      prefs;
WiFiClientSecure apiClient;          // kept open between commands
uint32_t         apiLastUse = 0;
String           accessToken;
String           refreshToken;
uint32_t         tokenValidUntil = 0;
int              volume          = -1;   // -1 = unknown
int              volumeBeforeMute = 30;
uint32_t         lastVolumeAction = 0;
String           deviceId = SPOTIFY_DEVICE_ID;   // can change at runtime
uint32_t         nextTrackPoll = 0;
// a failed start is retried later
String           pendingStartUri;
uint32_t         pendingStartNo    = 0;
uint8_t          pendingStartRound = 0;     // 0 = nothing pending
uint32_t         nextStartAttempt  = 0;
bool             shuffleOn = false;
uint8_t          connErrorsInRow = 0;
bool             logRequests = true;

// --- Display ---
Adafruit_SSD1306 display(128, 32, &Wire, -1);
bool             displayOk = false;
// The texts are written by spotifyTask and loop() and read by displayTask
SemaphoreHandle_t displayLock;
String           dispTitle;
String           dispArtist;
String           dispAlbum;
String           dispNote;
String           dispStatus;          // replaces "Starting...", e.g. "Waking Echo..."
String           dispStatusSmall;
volatile uint32_t noteUntil      = 0;
volatile int     dispVolume = -1;
volatile uint32_t overlayUntil  = 0;
volatile uint32_t shuffleOverlayUntil = 0;
volatile bool     dispShuffle = false;

// ===========================================================================
// WiFi
// ===========================================================================
void connectWifi() {
  Serial.print("Connecting to WiFi ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  // modem sleep off, otherwise requests get slow or fail
  WiFi.setSleep(false);
  Serial.print(" connected, IP: ");
  Serial.print(WiFi.localIP());
  Serial.printf(", signal %d dBm\n", (int)WiFi.RSSI());
}

// ===========================================================================
// Spotify
// ===========================================================================

// Minimal JSON value lookup, enough for the token response
String jsonValue(const String& json, const char* key) {
  String needle = String('"') + key + '"';
  int pos = json.indexOf(needle);
  if (pos < 0) return "";
  pos = json.indexOf(':', pos + needle.length());
  if (pos < 0) return "";
  pos++;
  while (pos < (int)json.length() && json[pos] == ' ') pos++;

  if (json[pos] == '"') {                       // string
    int stopAt = json.indexOf('"', pos + 1);
    return json.substring(pos + 1, stopAt);
  }
  int stopAt = pos;                             // number: up to comma or brace
  while (stopAt < (int)json.length() && json[stopAt] != ',' && json[stopAt] != '}') stopAt++;
  return json.substring(pos, stopAt);
}

// Gets a new access token with the refresh token (PKCE, no client secret).
// Happens about once an hour, so it uses its own short connection.
bool refreshAccessToken() {
  WiFiClientSecure client;
  client.setInsecure();   // prototype: certificate not checked, still encrypted
  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=refresh_token&refresh_token=" + refreshToken +
                "&client_id=" + SPOTIFY_CLIENT_ID;
  int status = http.POST(body);
  String response = (status > 0 && status != 204) ? http.getString() : "";
  http.end();

  if (status < 0) {
    // accounts.spotify.com not reachable, count it like any other
    // connection error so the self healing kicks in
    Serial.printf("Token refresh: no connection (%d)\n", status);
    connectionError();
    return false;
  }
  if (status != 200) {
    Serial.printf("Token refresh failed (%d): %s\n", status, response.c_str());
    showNote("Spotify login failed");
    return false;
  }
  connErrorsInRow = 0;

  accessToken = jsonValue(response, "access_token");
  long seconds = jsonValue(response, "expires_in").toInt();
  if (seconds <= 0) seconds = 3600;
  tokenValidUntil = millis() + (uint32_t)(seconds - 60) * 1000UL;   // 1 min margin

  // Spotify may send a new refresh token, keep it in flash
  String newVal = jsonValue(response, "refresh_token");
  if (newVal.length() > 0 && newVal != refreshToken) {
    refreshToken = newVal;
    prefs.putString("refresh", refreshToken);
  }
  Serial.println("Access token refreshed.");
  return true;
}

bool tokenReady() {
  // signed difference, safe when millis() overflows
  if (accessToken.length() > 0 && (int32_t)(tokenValidUntil - millis()) > 0) return true;
  return refreshAccessToken();
}

// --- Plain HTTP/1.1 on the kept-alive connection -------------------------
// Written by hand instead of HTTPClient to control keep-alive exactly and to
// never wait for data that will not come.

bool readLine(String& line, uint32_t deadline) {
  line = "";
  while ((int32_t)(deadline - millis()) > 0) {
    while (apiClient.available() > 0) {
      char c = (char)apiClient.read();
      if (c == '\n') {
        line.trim();                   // removes the '\r'
        return true;
      }
      line += c;
    }
    if (!apiClient.connected()) return false;
    delay(1);
  }
  return false;
}

// Reads exactly count bytes and appends them to dest
bool readExact(String& dest, size_t count, uint32_t deadline) {
  uint8_t buf[256];
  while (count > 0) {
    if ((int32_t)(deadline - millis()) <= 0) return false;
    if (apiClient.available() <= 0) {
      if (!apiClient.connected()) return false;
      delay(1);
      continue;
    }
    size_t want = count < sizeof(buf) ? count : sizeof(buf);
    int n = apiClient.read(buf, want);
    if (n <= 0) { delay(1); continue; }
    for (int i = 0; i < n; i++) dest += (char)buf[i];
    count -= n;
  }
  return true;
}

// One request to api.spotify.com. Returns the HTTP status, negative values
// are connection level errors.
int apiRequestOnce(const char* method, const String& path,
                   const String& body, String& response) {
  response = "";

  if (millis() - apiLastUse > CONN_IDLE_MS) apiClient.stop();

  if (!apiClient.connected()) {
    apiClient.stop();
    if (!apiClient.connect(API_HOST, 443, 5000)) {
      // print what could be the reason: DNS, heap or signal
      IPAddress ip;
      bool dnsOk = WiFi.hostByName(API_HOST, ip);
      Serial.printf("  Connect failed. DNS %s (%s), free heap %u, "
                    "largest block %u, WiFi %d dBm, status %d\n",
                    dnsOk ? "ok" : "ERROR", ip.toString().c_str(),
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                    (int)WiFi.RSSI(), (int)WiFi.status());
      return -1;
    }
  }
  while (apiClient.available() > 0) apiClient.read();          // drop leftovers

  String request = String(method) + " /v1" + path + " HTTP/1.1\r\n" +
                   "Host: " + API_HOST + "\r\n" +
                   "Authorization: Bearer " + accessToken + "\r\n" +
                   "Content-Type: application/json\r\n" +
                   "Content-Length: " + String(body.length()) + "\r\n" +
                   "Connection: keep-alive\r\n\r\n" +
                   body;
  if (apiClient.print(request) != request.length()) {
    apiClient.stop();
    return -3;
  }
  apiLastUse = millis();

  // status line, e.g. "HTTP/1.1 204 No Content"
  uint32_t deadline = millis() + RESPONSE_TIMEOUT_MS;
  String line;
  if (!readLine(line, deadline)) { apiClient.stop(); return -5; }
  int space = line.indexOf(' ');
  int status = (space > 0) ? line.substring(space + 1, space + 4).toInt() : 0;
  if (status <= 0) { apiClient.stop(); return -5; }

  // headers until the empty line
  long length = -1;
  bool chunked = false;
  bool closeConn = false;
  for (;;) {
    if (!readLine(line, deadline)) { apiClient.stop(); return -5; }
    if (line.length() == 0) break;
    line.toLowerCase();
    if (line.startsWith("content-length:")) {
      length = line.substring(15).toInt();
    } else if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") > 0) {
      chunked = true;
    } else if (line.startsWith("connection:") && line.indexOf("close") > 0) {
      closeConn = true;
    }
  }

  // body
  bool ok = true;
  if (status == 204 || status == 304) {
    // no body
  } else if (chunked) {
    response.reserve(4096);
    // chunks: size in hex, CRLF, data, CRLF. Size 0 ends the body.
    for (;;) {
      if (!readLine(line, deadline)) { ok = false; break; }
      long chunk = strtol(line.c_str(), nullptr, 16);
      if (chunk <= 0) { readLine(line, deadline); break; }
      if (!readExact(response, (size_t)chunk, deadline)) { ok = false; break; }
      readLine(line, deadline);
    }
  } else if (length >= 0) {
    response.reserve(length + 1);
    ok = readExact(response, (size_t)length, deadline);
  } else {
    // no length and not chunked: server closes the connection at the end
    while (apiClient.connected() || apiClient.available() > 0) {
      if ((int32_t)(deadline - millis()) <= 0) break;
      if (apiClient.available() > 0) response += (char)apiClient.read();
      else delay(1);
    }
    closeConn = true;
  }

  if (!ok || closeConn) apiClient.stop();
  return status;
}

// Counts a connection error and steps in when they pile up.
// Returns true if WiFi was just reconnected, then a retry makes sense.
bool connectionError() {
  connErrorsInRow++;
  showNote("No connection to Spotify");
  if (connErrorsInRow >= 2 * ERRORS_UNTIL_WIFI_RESET) {
    Serial.println("Connection still down. Restarting the ESP32...");
    showNote("Restarting...");
    delay(1000);
    ESP.restart();
  }
  if (connErrorsInRow >= ERRORS_UNTIL_WIFI_RESET) {
    // fixes stuck DNS and connections that fail after a long uptime
    Serial.println("Several connection errors. Reconnecting WiFi...");
    apiClient.stop();
    WiFi.disconnect();
    delay(500);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t waitUntil = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && (int32_t)(waitUntil - millis()) > 0) delay(250);
    bool back = WiFi.status() == WL_CONNECTED;
    Serial.printf("WiFi %s\n", back ? "reconnected" : "still down");
    return back;
  }
  return false;
}

// Request with timing and one retry on a fresh connection
int apiRequest(const char* method, const String& path,
               const String& body, String& response) {
  uint32_t start = millis();
  spotifyAliveMs = millis();
  int status = apiRequestOnce(method, path, body, response);
  if (status < 0) {
    Serial.printf("  Connection error %d, retrying on a fresh connection\n", status);
    apiClient.stop();
    status = apiRequestOnce(method, path, body, response);
  }

  if (status < 0) {
    if (connectionError()) {
      status = apiRequestOnce(method, path, body, response);
      if (status >= 0) connErrorsInRow = 0;
    }
  } else {
    connErrorsInRow = 0;
  }

  // log only the part before '?', the device ID is long
  int q = path.indexOf('?');
  String shortPath = (q >= 0) ? path.substring(0, q) : path;
  if (logRequests || status < 0 || status >= 300) {
    Serial.printf("  %s %s -> %d (%lu ms)\n", method, shortPath.c_str(), status,
                  (unsigned long)(millis() - start));
  }
  if (status >= 300 && response.length() > 0) {
    Serial.printf("    Spotify says: %s\n", response.substring(0, 200).c_str());
  }
  return status;
}

// Gets the device list, prints it and looks for the Echo by name.
// true if found (deviceId is up to date then).
bool findDevice() {
  String response;
  int status = apiRequest("GET", "/me/player/devices", "", response);
  if (status != 200) return false;

  Serial.println("Devices Spotify knows right now:");
  bool found = false;
  int pos = 0;
  // every device is a flat object in braces
  for (;;) {
    int objStart = response.indexOf('{', pos + 1);
    if (objStart < 0) break;
    int objEnd = response.indexOf('}', objStart);
    if (objEnd < 0) break;
    String obj = response.substring(objStart, objEnd + 1);
    pos = objEnd;
    String id   = jsonValue(obj, "id");
    String name = jsonValue(obj, "name");
    if (id.length() == 0) continue;
    Serial.printf("  %s  (%s)\n", name.c_str(), id.c_str());
    String nameLower = name;
    nameLower.toLowerCase();
    if (!found && nameLower.indexOf(DEVICE_NAME_PART) >= 0) {
      found = true;
      if (id != deviceId) {
        Serial.println("  -> new Echo ID saved");
        deviceId = id;
        prefs.putString("geraet", deviceId);
      }
    }
  }
  if (!found) {
    Serial.println("  Echo not in the list, Spotify cannot reach it right now.");
    showNote("Echo not reachable");
    Serial.println("  Tip: say \"Alexa, play Spotify\" once, then try again.");
  }
  return found;
}

// Sends a player command.
//   method   "PUT" or "POST"
//   command  play, pause, volume, next, previous, shuffle
//   params   extra query, e.g. "&volume_percent=40"
bool playerCommand(const char* method, const char* command,
                   const String& params, const String& body) {
  if (!tokenReady()) return false;
  String path = String("/me/player/") + command + "?device_id=" + deviceId + params;
  String response;
  int status = apiRequest(method, path, body, response);
  if (status == 401 && refreshAccessToken()) {
    status = apiRequest(method, path, body, response);
  }
  if (status == 404) {
    // device ID unknown, the Echo may have a new one
    Serial.println("Spotify does not know this device ID, looking for the Echo...");
    if (findDevice()) {
      path = String("/me/player/") + command + "?device_id=" + deviceId + params;
      status = apiRequest(method, path, body, response);
    }
  }
  if (status == 200 || status == 202 || status == 204) return true;

  Serial.printf("Spotify error on %s (%d)\n", command, status);
  return false;
}

// Starts an album or playlist at the first track, position 0
bool play(const String& uri) {
  String body;
  if (uri.startsWith("spotify:track:")) {
    body = "{\"uris\":[\"" + uri + "\"],\"position_ms\":0}";
  } else {
    body = "{\"context_uri\":\"" + uri +
           "\",\"offset\":{\"position\":0},\"position_ms\":0}";
  }
  return playerCommand("PUT", "play", "", body);
}

bool resume()        { return playerCommand("PUT",  "play",     "", "{}"); }
bool pausePlayback() { return playerCommand("PUT",  "pause",    "", "{}"); }
bool nextTrack()     { return playerCommand("POST", "next",     "", "{}"); }
bool previousTrack() { return playerCommand("POST", "previous", "", "{}"); }
bool shuffleOff() {
  bool ok = playerCommand("PUT", "shuffle", "&state=false", "{}");
  if (ok) shuffleOn = false;
  return ok;
}

bool setVolume(int percent) {
  return playerCommand("PUT", "volume", "&volume_percent=" + String(percent), "{}");
}

// Current Echo volume from Spotify, -1 on error
int readVolume() {
  if (!tokenReady()) return -1;
  String response;
  int status = apiRequest("GET", "/me/player", "", response);
  if (status == 401 && refreshAccessToken()) status = apiRequest("GET", "/me/player", "", response);
  if (status != 200) return -1;                     // 204 = nothing active
  String val = jsonValue(response, "volume_percent");
  if (val.length() == 0 || val == "null") return -1;
  return val.toInt();
}

// Transfers playback to the Echo. This often wakes it up when commands
// silently go nowhere.
bool wakeEcho() {
  if (!tokenReady()) return false;
  String response;
  String body = "{\"device_ids\":[\"" + deviceId + "\"],\"play\":false}";
  int status = apiRequest("PUT", "/me/player", body, response);
  if (status == 401 && refreshAccessToken()) status = apiRequest("PUT", "/me/player", body, response);
  return status == 200 || status == 202 || status == 204;
}

// 1 = Echo is playing, 0 = not playing, -1 = request failed
int checkEchoPlaying() {
  if (!tokenReady()) return -1;
  String response;
  int status = apiRequest("GET", "/me/player", "", response);
  if (status == 401 && refreshAccessToken()) status = apiRequest("GET", "/me/player", "", response);
  if (status == 204) return 0;
  if (status != 200) return -1;

  JsonDocument filter;
  filter["is_playing"] = true;
  filter["device"]["id"] = true;
  filter["device"]["name"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, response, DeserializationOption::Filter(filter))) return -1;

  bool   playing = doc["is_playing"] | false;
  String id      = doc["device"]["id"] | "";
  String name    = doc["device"]["name"] | "";
  String lower   = name;
  lower.toLowerCase();
  bool onEcho = (id == deviceId) || lower.indexOf(DEVICE_NAME_PART) >= 0;
  if (playing && !onEcho) {
    Serial.printf("  Spotify is playing on \"%s\", not on the Echo.\n", name.c_str());
  }
  return (playing && onEcho) ? 1 : 0;
}

bool waitForEcho() {
  uint32_t stopAt = millis() + START_CHECK_MS;
  while ((int32_t)(stopAt - millis()) > 0) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    spotifyAliveMs = millis();
    if (!shouldPlay) return false;                  // arm was lifted meanwhile
    if (checkEchoPlaying() == 1) return true;
  }
  return false;
}

// Starts a record and checks that the Echo really plays. If not, wake it,
// and if that does not help, look it up in the device list.
bool startAndVerify(const String& uri) {
  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    if (!shouldPlay) return false;
    if (attempt == 2) {
      Serial.println("Echo is not playing. Trying to wake it...");
      setStatus("Waking Echo...", "");
      wakeEcho();
      vTaskDelay(pdMS_TO_TICKS(1500));
    } else if (attempt == 3) {
      Serial.println("Still nothing. Looking for the Echo in the device list...");
      setStatus("Finding Echo...", "");
      if (!findDevice()) {
        setStatus("Echo offline", "Alexa, play Spotify");
        return false;
      }
      wakeEcho();
      vTaskDelay(pdMS_TO_TICKS(1500));
    }
    if (play(uri)) {
      shuffleOff();
      if (waitForEcho()) return true;
    }
  }
  setStatus("Echo stuck", "not playing");
  return false;
}

// Remembers a failed start for a later retry
void startFailed(const String& uri, uint32_t num) {
  if (pendingStartNo != num) pendingStartRound = 0;  // new record: count again
  pendingStartUri = uri;
  pendingStartNo  = num;
  pendingStartRound++;
  if (pendingStartRound <= START_ROUNDS) {
    nextStartAttempt = millis() + START_ROUND_PAUSE_MS;
    Serial.printf("Start failed. Retry %u of %u in %lu s.\n",
                  (unsigned)pendingStartRound, (unsigned)START_ROUNDS,
                  (unsigned long)(START_ROUND_PAUSE_MS / 1000));
    xSemaphoreTake(displayLock, portMAX_DELAY);
    String small = dispStatusSmall;
    xSemaphoreGive(displayLock);
    if (small.length() == 0 || small == "not playing") {
      setStatus(statusLine(), "retrying soon");
    }
  } else {
    pendingStartRound = 0;                          // give up
    Serial.println("Start failed for good. Lift and lower the arm to try again.");
    setStatus(statusLine(), "lift and lower arm");
  }
}

// ---------------------------------------------------------------------------
// spotifyTask: takes the pending jobs and works through them
// ---------------------------------------------------------------------------
void spotifyTask(void* params) {
  apiClient.setInsecure();     // prototype: certificate not checked

  for (;;) {
    spotifyAliveMs = millis();
    xSemaphoreTake(jobLock, portMAX_DELAY);
    Jobs a = pendingJobs;
    pendingJobs = Jobs();
    xSemaphoreGive(jobLock);

    bool hasWork = a.playback != PB_NONE || a.skips != 0 ||
                   a.volumeChange != 0 || a.toggleMute || a.toggleShuffle;
    // retry a failed start after the pause, as long as the same record is
    // on and the arm is down
    if (!hasWork && pendingStartRound > 0) {
      if (!shouldPlay || pendingStartNo != recordNo || startedRecordNo == recordNo) {
        pendingStartRound = 0;
      } else if ((int32_t)(millis() - nextStartAttempt) >= 0) {
        a.playback = PB_START;
        a.uri      = pendingStartUri;
        a.recordNo = pendingStartNo;
        hasWork    = true;
        Serial.printf("Retry %u of %u\n", (unsigned)pendingStartRound, (unsigned)START_ROUNDS);
      }
    }

    if (!hasWork) {
      // idle: poll the current track every few seconds while playing
      if (shouldPlay && startedRecordNo == recordNo && WiFi.status() == WL_CONNECTED &&
          (int32_t)(millis() - nextTrackPoll) >= 0) {
        fetchTrack();
        nextTrackPoll = millis() + TRACK_POLL_MS;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("No WiFi, job dropped.");
      continue;
    }

    // --- 1. playback ---
    switch (a.playback) {
      case PB_START:
        Serial.printf("Spotify: start from the beginning %s\n", a.uri.c_str());
        setStatus("Starting...", "");
        if (startAndVerify(a.uri)) {
          startedRecordNo = a.recordNo;
          pendingStartRound = 0;
          setStatus("", "");
          int actual = readVolume();
          if (actual >= 0) {
            volume = actual;
            dispVolume = actual;
            lastVolumeAction = millis();
          }
        } else if (shouldPlay) {
          startFailed(a.uri, a.recordNo);
        }
        break;
      case PB_RESUME:
        Serial.println("Spotify: resume");
        if (!resume()) {
          // after a long pause the Echo is sometimes asleep
          Serial.println("Resume failed. Trying to wake the Echo...");
          wakeEcho();
          vTaskDelay(pdMS_TO_TICKS(1500));
          if (!resume()) showNote("Echo not responding");
        }
        break;
      case PB_PAUSE:
        Serial.println("Spotify: pause");
        pausePlayback();
        break;
      default:
        break;
    }

    // --- 2. skip tracks ---
    bool skipped = a.skips != 0;
    while (a.skips > 0) { Serial.println("Spotify: next track");     nextTrack();     a.skips--; }
    while (a.skips < 0) { Serial.println("Spotify: previous track"); previousTrack(); a.skips++; }
    // skipping starts playback on some devices, so pause again if the arm is up
    if (skipped && !shouldPlay) pausePlayback();
    // the Echo needs a moment before Spotify reports the new track
    if (a.playback == PB_START || a.playback == PB_RESUME || skipped) {
      nextTrackPoll = millis() + 1000;
      if (skipped && !shouldPlay) fetchTrack();      // show it right away while paused
    }

    // --- 3. volume ---
    if (a.volumeChange != 0 || a.toggleMute) {
      if (volume < 0 || millis() - lastVolumeAction > VOLUME_REREAD_MS) {
        int actual = readVolume();
        if (actual >= 0) volume = actual;
        else if (volume < 0) volume = 30;            // fallback if unknown
      }
      int newVal = volume;
      if (a.toggleMute) {
        if (volume > 0) { volumeBeforeMute = volume; newVal = 0; }
        else            { newVal = volumeBeforeMute; }
      }
      newVal = constrain(newVal + a.volumeChange, 0, 100);
      if (newVal != volume && setVolume(newVal)) volume = newVal;
      lastVolumeAction = millis();
      Serial.printf("Volume now %d %%\n", volume);
      dispVolume = volume;
      overlayUntil = millis() + OVERLAY_MS;
    }

    // --- 4. shuffle ---
    if (a.toggleShuffle) {
      bool newVal = !shuffleOn;
      if (playerCommand("PUT", "shuffle", newVal ? "&state=true" : "&state=false", "{}")) {
        shuffleOn = newVal;
      }
      Serial.printf("Shuffle %s\n", shuffleOn ? "on" : "off");
      dispShuffle = shuffleOn;
      shuffleOverlayUntil = millis() + OVERLAY_MS;
    }
  }
}

// --- add jobs (called from loop()) ---
void queuePlayback(Playback w, const String& uri, uint32_t num) {
  xSemaphoreTake(jobLock, portMAX_DELAY);
  pendingJobs.playback = w;                         // last request wins
  pendingJobs.uri = uri;
  pendingJobs.recordNo = num;
  xSemaphoreGive(jobLock);
}

void queueSkip(int8_t dir) {
  xSemaphoreTake(jobLock, portMAX_DELAY);
  int newVal = pendingJobs.skips + dir;
  pendingJobs.skips = constrain(newVal, -5, 5);
  xSemaphoreGive(jobLock);
}

void queueVolume(int16_t delta) {
  xSemaphoreTake(jobLock, portMAX_DELAY);
  int newVal = pendingJobs.volumeChange + delta;
  pendingJobs.volumeChange = constrain(newVal, -100, 100);
  xSemaphoreGive(jobLock);
}

void queueShuffle() {
  xSemaphoreTake(jobLock, portMAX_DELAY);
  pendingJobs.toggleShuffle = !pendingJobs.toggleShuffle;
  xSemaphoreGive(jobLock);
}

void queueMute() {
  xSemaphoreTake(jobLock, portMAX_DELAY);
  pendingJobs.toggleMute = !pendingJobs.toggleMute;
  xSemaphoreGive(jobLock);
}

// ===========================================================================
// NFC tag
// ===========================================================================

// Is a tag in the field? WUPA also finds tags that were halted after reading.
bool tagInField() {
  uint8_t response[2];
  uint8_t size = sizeof(response);
  MFRC522::StatusCode status = rfid.PICC_WakeupA(response, &size);
  return status == MFRC522::STATUS_OK || status == MFRC522::STATUS_COLLISION;
}

// Reads the 144 bytes of user memory (pages 4 to 39)
bool readUserData(uint8_t* dest) {
  for (uint16_t offset = 0; offset < USER_DATA_LEN; offset += 16) {
    uint8_t buf[18];                            // 16 data + 2 CRC
    uint8_t size = sizeof(buf);
    uint8_t page = FIRST_PAGE + offset / 4;     // one read returns 4 pages
    if (rfid.MIFARE_Read(page, buf, &size) != MFRC522::STATUS_OK) return false;
    memcpy(dest + offset, buf, 16);
  }
  return true;
}

// Returns the first URI record of the NDEF message.
//   TLV blocks: [type][length][value], 0x03 = NDEF, 0xFE = end, 0x00 = padding
//   NDEF record: [header][type len][payload len][type][payload], type 'U' = URI
String findUrl(const uint8_t* data, uint16_t length) {
  uint16_t i = 0;
  while (i < length) {
    uint8_t type = data[i];
    if (type == 0x00) { i++; continue; }
    if (type == 0xFE) return "";
    if (i + 1 >= length) return "";
    uint16_t blockLen = data[i + 1];
    if (blockLen == 0xFF) return "";              // 3 byte length, not used on NTAG213
    uint16_t start = i + 2;
    if (start + blockLen > length) return "";
    if (type != 0x03) { i = start + blockLen; continue; }

    uint16_t p = start;
    uint16_t stopAt = start + blockLen;
    while (p < stopAt) {
      uint8_t header = data[p++];
      bool shortRec = header & 0x10;              // SR: 1 byte payload length
      bool hasId    = header & 0x08;              // IL: ID field present
      bool last     = header & 0x40;              // ME: last record
      uint8_t tnf   = header & 0x07;              // 1 = well-known type

      if (p >= stopAt) return "";
      uint8_t typeLen = data[p++];
      uint32_t payloadLen;
      if (shortRec) {
        if (p >= stopAt) return "";
        payloadLen = data[p++];
      } else {
        if (p + 4 > stopAt) return "";
        payloadLen = ((uint32_t)data[p] << 24) | ((uint32_t)data[p + 1] << 16) |
                     ((uint32_t)data[p + 2] << 8) | data[p + 3];
        p += 4;
      }
      uint8_t idLen = 0;
      if (hasId) {
        if (p >= stopAt) return "";
        idLen = data[p++];
      }
      if ((uint32_t)p + typeLen + idLen + payloadLen > stopAt) return "";
      const uint8_t* typeField = data + p;
      const uint8_t* payload   = data + p + typeLen + idLen;

      if (tnf == 0x01 && typeLen == 1 && typeField[0] == 'U' && payloadLen >= 1) {
        const char* prefix = "";                  // URI identifier code
        switch (payload[0]) {
          case 0x01: prefix = "http://www.";  break;
          case 0x02: prefix = "https://www."; break;
          case 0x03: prefix = "http://";      break;
          case 0x04: prefix = "https://";     break;
        }
        String url = prefix;
        for (uint32_t k = 1; k < payloadLen; k++) url += (char)payload[k];
        return url;
      }
      p += typeLen + idLen + payloadLen;
      if (last) break;
    }
    return "";
  }
  return "";
}

// https://open.spotify.com/album/ABC?si=xyz  ->  spotify:album:ABC
String linkToUri(const String& link) {
  if (link.startsWith("spotify:")) return link;
  const char* marker = "open.spotify.com/";
  int pos = link.indexOf(marker);
  if (pos < 0) return "";
  String rest = link.substring(pos + strlen(marker));
  if (rest.startsWith("intl-")) {                 // strip e.g. "intl-de/"
    int slash = rest.indexOf('/');
    if (slash < 0) return "";
    rest = rest.substring(slash + 1);
  }
  int slash = rest.indexOf('/');
  if (slash < 0) return "";
  String kind = rest.substring(0, slash);
  String id   = rest.substring(slash + 1);
  if (kind != "album" && kind != "playlist" && kind != "track") return "";
  int stopAt = id.indexOf('?');
  if (stopAt >= 0) id = id.substring(0, stopAt);
  stopAt = id.indexOf('/');
  if (stopAt >= 0) id = id.substring(0, stopAt);
  if (id.length() == 0) return "";
  return "spotify:" + kind + ":" + id;
}

String readUriFromTag(bool& readError) {
  readError = true;
  if (!rfid.PICC_ReadCardSerial()) return "";
  uint8_t data[USER_DATA_LEN];
  bool ok = readUserData(data);
  rfid.PICC_HaltA();
  if (!ok) return "";
  readError = false;
  String url = findUrl(data, USER_DATA_LEN);
  if (url.length() == 0) return "";
  Serial.print("Link on the record: ");
  Serial.println(url);
  return linkToUri(url);
}

// ===========================================================================
// Inputs (run in loop)
// ===========================================================================

// Debounce, returns true once per new press
bool buttonPressed(Button& t) {
  bool raw = (digitalRead(t.pin) == LOW);
  if (raw != t.lastRaw) {
    t.lastRaw = raw;
    t.lastChange = millis();
  }
  if (millis() - t.lastChange > DEBOUNCE_MS && raw != t.pressed) {
    t.pressed = raw;
    return t.pressed;                             // on press only, not on release
  }
  return false;
}

// Quadrature decoding with a lookup table: old and new state of the two
// lines give +1, -1 or 0 (invalid transition / bounce).
void IRAM_ATTR encoderISR() {
  static const int8_t TABLE[16] = { 0, -1,  1,  0,
                                    1,  0,  0, -1,
                                   -1,  0,  0,  1,
                                    0,  1, -1,  0 };
  uint8_t ab = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  portENTER_CRITICAL_ISR(&encLock);
  encState = ((encState << 2) | ab) & 0x0F;
  encCount += TABLE[encState];
  portEXIT_CRITICAL_ISR(&encLock);
}

void checkEncoder() {
  // take only full detents out of the counter
  portENTER_CRITICAL(&encLock);
  int32_t detents = encCount / ENC_TRANSITIONS_PER_DETENT;
  encCount -= detents * ENC_TRANSITIONS_PER_DETENT;
  portEXIT_CRITICAL(&encLock);

  if (detents != 0) {
    if (ENC_REVERSE) detents = -detents;
    Serial.printf("Encoder: %+ld\n", (long)detents);
    queueVolume(detents * VOLUME_STEP);
  }
  if (buttonPressed(btnEncoder)) {
    Serial.println("Encoder pressed: mute on/off");
    queueMute();
  }
}

void checkButtons() {
  // only skip once the current record has started, otherwise it would skip
  // in some old playback
  bool maySkip = recordPresent && startedRecordNo == recordNo;
  if (buttonPressed(btnNext)) {
    if (maySkip) queueSkip(+1);
    else Serial.println("Next ignored: record not started yet.");
  }
  if (buttonPressed(btnPrev)) {
    if (maySkip) queueSkip(-1);
    else Serial.println("Previous ignored: record not started yet.");
  }
  if (buttonPressed(btnShuffle)) {
    if (maySkip) queueShuffle();
    else Serial.println("Shuffle ignored: record not started yet.");
  }
}

void checkTonearm() {
  buttonPressed(btnArm);                          // updates the debounced state
  bool down = (btnArm.pressed == TONEARM_PRESSED_MEANS_DOWN);
  if (down != armDown) {
    armDown = down;
    Serial.println(armDown ? "Tonearm down." : "Tonearm up.");
  }
}

void checkRecord() {
  if (tagInField()) {
    missCount = 0;
    if (!recordRead) {
      bool readError;
      String uri = readUriFromTag(readError);
      if (readError) {
        // tag is there but could not be read, report at most once a second
        static uint32_t lastReport = 0;
        if (millis() - lastReport > 1000) {
          lastReport = millis();
          Serial.println("Tag found, read failed. Retrying...");
        }
      }
      if (!readError) {                           // on error the next poll tries again
        recordRead    = true;
        recordPresent = true;
        recordNo++;
        currentUri = uri;
        Serial.println("Record detected.");
        if (uri.length() == 0) Serial.println("Record has no Spotify link.");
        else if (!armDown)     Serial.println("Record detected. Lower the tonearm to start.");
      }
    } else {
      rfid.PICC_HaltA();                          // still there, halt it again
    }
  } else {
    if (missCount < 255) missCount++;
    if (recordPresent && missCount >= MISSES_UNTIL_GONE) {
      recordPresent = false;
      recordRead    = false;
      currentUri    = "";
      Serial.println("Record removed.");
      setTrack("", "", "");
    } else if (!recordPresent) {
      recordRead = false;
    }
  }
}

// Decides from record and tonearm whether music should play
void updatePlayback() {
  bool should = recordPresent && currentUri.length() > 0 && armDown;
  shouldPlay = should;
  motorShouldRun = should;                        // platter reacts immediately
  if (should == lastShouldPlay) return;
  lastShouldPlay = should;

  if (should) {
    if (startedRecordNo == recordNo) {
      queuePlayback(PB_RESUME, "", recordNo);     // same record: continue
    } else {
      queuePlayback(PB_START, currentUri, recordNo);   // new record: from the start
    }
  } else {
    queuePlayback(PB_PAUSE, "", recordNo);
  }
}

// ===========================================================================
// Track info and display
// ===========================================================================

void fetchTrack() {
  if (!tokenReady()) return;
  String response;
  logRequests = false;                            // every 5 s is too much for the log
  int status = apiRequest("GET", "/me/player/currently-playing", "", response);
  if (status == 401 && refreshAccessToken()) {
    status = apiRequest("GET", "/me/player/currently-playing", "", response);
  }
  logRequests = true;
  if (status != 200) return;                      // 204 = nothing playing

  // the response is large, the filter keeps only the three fields we need
  JsonDocument filter;
  filter["item"]["name"] = true;
  filter["item"]["album"]["name"] = true;
  filter["item"]["artists"][0]["name"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("Track info not readable: %s\n", err.c_str());
    return;
  }
  const char* title  = doc["item"]["name"] | "";
  const char* artist = doc["item"]["artists"][0]["name"] | "";
  const char* album  = doc["item"]["album"]["name"] | "";

  xSemaphoreTake(displayLock, portMAX_DELAY);
  bool newVal = (dispTitle != title);
  xSemaphoreGive(displayLock);
  if (newVal && strlen(title) > 0) Serial.printf("Playing: %s - %s\n", artist, title);
  setTrack(title, artist, album);
}

void setTrack(const String& title, const String& artist, const String& album) {
  xSemaphoreTake(displayLock, portMAX_DELAY);
  dispTitle = title;
  dispArtist = artist;
  dispAlbum = album;
  xSemaphoreGive(displayLock);
}

void setStatus(const String& big, const String& small) {
  xSemaphoreTake(displayLock, portMAX_DELAY);
  dispStatus = big;
  dispStatusSmall = small;
  xSemaphoreGive(displayLock);
}

String statusLine() {
  xSemaphoreTake(displayLock, portMAX_DELAY);
  String t = dispStatus;
  xSemaphoreGive(displayLock);
  return t;
}

void showNote(const String& text) {
  xSemaphoreTake(displayLock, portMAX_DELAY);
  dispNote = text;
  xSemaphoreGive(displayLock);
  noteUntil = millis() + NOTE_MS;
}

// Spotify sends UTF-8, the built-in display font is CP437. Common accented
// characters are converted, everything else becomes '?'.
String toDisplay(const String& text) {
  String r;
  for (unsigned int i = 0; i < text.length(); i++) {
    uint8_t c = (uint8_t)text[i];
    if (c < 0x80) { r += (char)c; continue; }
    if (c == 0xC3 && i + 1 < text.length()) {
      uint8_t d = (uint8_t)text[++i];
      uint8_t z = '?';
      switch (d) {
        case 0xA4: z = 0x84; break;   // ä
        case 0xB6: z = 0x94; break;   // ö
        case 0xBC: z = 0x81; break;   // ü
        case 0x84: z = 0x8E; break;   // Ä
        case 0x96: z = 0x99; break;   // Ö
        case 0x9C: z = 0x9A; break;   // Ü
        case 0x9F: z = 0xE1; break;   // ß
        case 0xA9: z = 0x82; break;   // é
        case 0xA8: z = 0x8A; break;   // è
        case 0xA1: z = 0xA0; break;   // á
        case 0xB1: z = 0xA4; break;   // ñ
        case 0xA7: z = 0x87; break;   // ç
      }
      r += (char)z;
      continue;
    }
    // other multi byte character
    r += '?';
    while (i + 1 < text.length() && (((uint8_t)text[i + 1]) & 0xC0) == 0x80) i++;
  }
  return r;
}

// Draws one line. Scrolls from right to left if it is too long.
//   size 1: 6 x 8 px per character, 21 per line
//   size 2: 12 x 16 px per character, 10 per line
void drawLine(int16_t y, const String& text, uint32_t now, uint8_t size) {
  display.setTextSize(size);
  int16_t charWidth = 6 * size;
  int16_t width = text.length() * charWidth;
  if (width <= 128) {
    display.setCursor(0, y);
    display.print(text);
    display.setTextSize(1);
    return;
  }
  String loopText = text + "    ";                // gap before it repeats
  int16_t total = loopText.length() * charWidth;
  // stand still for 2 s, then scroll at 25 px/s
  uint32_t t = now % (2000 + (uint32_t)total * 40);
  int16_t offset = (t < 2000) ? 0 : (int16_t)((t - 2000) / 40);
  display.setCursor(-offset, y);
  display.print(loopText);
  display.print(loopText);                        // second copy closes the gap
  display.setTextSize(1);
}

void drawScreen() {
  uint32_t now = millis();

  xSemaphoreTake(displayLock, portMAX_DELAY);
  String title    = toDisplay(dispTitle);
  String artist   = toDisplay(dispArtist);
  String album    = toDisplay(dispAlbum);
  String note     = toDisplay(dispNote);
  String status   = toDisplay(dispStatus);
  String statusSm = toDisplay(dispStatusSmall);
  xSemaphoreGive(displayLock);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // 1. volume just changed: big number and bar
  if ((int32_t)(overlayUntil - now) > 0) {
    int vol = dispVolume;
    display.setTextSize(2);
    display.setCursor(0, 0);
    if (vol == 0) display.print("Muted");
    else          display.printf("Vol %d%%", vol);
    display.drawRect(0, 22, 128, 10, SSD1306_WHITE);
    if (vol > 0) display.fillRect(2, 24, (124 * vol) / 100, 6, SSD1306_WHITE);
    display.display();
    return;
  }

  // 1b. shuffle just toggled
  if ((int32_t)(shuffleOverlayUntil - now) > 0) {
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.print("Shuffle");
    display.setCursor(0, 16);
    display.print(dispShuffle ? "on" : "off");
    display.display();
    return;
  }

  // 2. note, e.g. "Echo not reachable"
  if ((int32_t)(noteUntil - now) > 0 && note.length() > 0) {
    display.setCursor(0, 0);
    display.print("Note");
    drawLine(12, note, now, 1);
    display.display();
    return;
  }

  // 3. normal screen
  bool started = recordPresent && startedRecordNo == recordNo;
  if (!recordPresent) {
    drawLine(0,  "Put on", now, 2);
    drawLine(16, "a record", now, 2);
  } else if (!started && !armDown) {
    drawLine(0,  "Lower", now, 2);
    drawLine(16, "the arm", now, 2);
  } else if (title.length() == 0) {
    if (!armDown) {
      drawLine(0, "Pause", now, 2);
    } else {
      // what is happening during the start, e.g. "Waking Echo..." or an error
      drawLine(0, status.length() > 0 ? status : String("Starting..."), now, 2);
      if (statusSm.length() > 0) drawLine(20, statusSm, now, 1);
    }
  } else {
    drawLine(0, title, now, 2);                   // title big, 16 px
    if (dispVolume == 0) {
      drawLine(16, "Muted", now, 2);
    } else {
      drawLine(16, album, now, 1);                // album and artist small, 8 px each
      drawLine(24, artist, now, 1);
    }
  }
  display.display();
}

// Simple screen while the tasks are not running yet
void showBoot(const char* line1, const char* line2) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(line1);
  display.setCursor(0, 12);
  display.print(line2);
  display.display();
}

void displayTask(void* params) {
  for (;;) {
    drawScreen();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ===========================================================================
// Stepper motor
// ===========================================================================
void motorTask(void* params) {
  // half step sequence, one bit per line, IN1 to IN4 from the left
  const uint8_t PATTERN[8] = { 0b1000, 0b1100, 0b0100, 0b0110,
                               0b0010, 0b0011, 0b0001, 0b1001 };
  uint8_t step = 0;
  bool coilsOn = false;
  TickType_t lastStep = xTaskGetTickCount();

  for (;;) {
    if (motorShouldRun) {
      for (uint8_t i = 0; i < 4; i++) {
        digitalWrite(PIN_MOTOR[i], (PATTERN[step] >> (3 - i)) & 1);
      }
      if (MOTOR_FULL_STEP) {                      // only the two-coil patterns
        step = ((step | 1) + (MOTOR_REVERSE ? 6 : 2)) & 7;
      } else {
        step = (step + (MOTOR_REVERSE ? 7 : 1)) & 7;
      }
      coilsOn = true;
      vTaskDelayUntil(&lastStep, pdMS_TO_TICKS(MOTOR_STEP_MS));
    } else {
      if (coilsOn) {                              // no current through the coils at standstill
        for (uint8_t i = 0; i < 4; i++) digitalWrite(PIN_MOTOR[i], LOW);
        coilsOn = false;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      lastStep = xTaskGetTickCount();
    }
  }
}

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(2000);   // gives the serial monitor time to connect
  Serial.println();
  Serial.println("=== RFID turntable, firmware 2.2 ===");

  // --- display ---
  displayLock = xSemaphoreCreateMutex();
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setClock(400000);
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (displayOk) {
    display.cp437(true);                          // correct table for umlauts
    display.setTextWrap(false);
    showBoot("Turntable", "Starting...");
  } else {
    Serial.println("Display not found. Check wiring (SDA 21, SCL 4).");
  }

  // --- motor ---
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(PIN_MOTOR[i], OUTPUT);
    digitalWrite(PIN_MOTOR[i], LOW);
  }

  // --- buttons, tonearm, encoder ---
  pinMode(PIN_NEXT,    INPUT_PULLUP);
  pinMode(PIN_PREV,    INPUT_PULLUP);
  pinMode(PIN_SHUFFLE, INPUT_PULLUP);
  pinMode(PIN_TONEARM, INPUT_PULLUP);
  pinMode(PIN_ENC_SW,  INPUT_PULLUP);
  pinMode(PIN_ENC_CLK, INPUT);                    // pull-ups are on the KY-040 board
  pinMode(PIN_ENC_DT,  INPUT);
  encState = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT),  encoderISR, CHANGE);

  // --- RFID reader ---
  SPI.begin();
  rfid.PCD_Init();
  delay(50);
  uint8_t version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("ERROR: RC522 not responding, check wiring.");
  } else {
    Serial.printf("RC522 found (version 0x%02X).\n", version);
  }

  // --- refresh token ---
  // First boot takes it from secrets.h, after that from flash, because
  // Spotify can replace it. A new value in secrets.h overrides flash.
  prefs.begin("spotify", false);
  String initial = prefs.getString("startwert", "");
  if (initial != SPOTIFY_REFRESH_TOKEN) {
    refreshToken = SPOTIFY_REFRESH_TOKEN;
    prefs.putString("startwert", SPOTIFY_REFRESH_TOKEN);
    prefs.putString("refresh", refreshToken);
    Serial.println("Refresh token taken from secrets.h.");
  } else {
    refreshToken = prefs.getString("refresh", SPOTIFY_REFRESH_TOKEN);
  }

  // --- Echo device ID ---
  // An ID found automatically earlier wins, unless secrets.h has a new one
  // (same idea as for the refresh token)
  if (prefs.getString("geraetStart", "") != SPOTIFY_DEVICE_ID) {
    prefs.putString("geraetStart", SPOTIFY_DEVICE_ID);
    prefs.remove("geraet");
  }
  deviceId = prefs.getString("geraet", SPOTIFY_DEVICE_ID);

  // --- WiFi and first token ---
  showBoot("Connecting WiFi...", WIFI_SSID);
  connectWifi();
  showBoot("WiFi ok", "Spotify login...");
  if (!refreshAccessToken()) {
    Serial.println("Spotify login failed. Check client ID and refresh token.");
    showBoot("Spotify login", "failed");
    delay(3000);
  }

  // --- tasks ---
  jobLock = xSemaphoreCreateMutex();
  // Spotify on core 0 (where WiFi runs), large stack for TLS.
  // Motor on core 1 with a higher priority than loop().
  spotifyAliveMs = millis();
  xTaskCreatePinnedToCore(spotifyTask, "spotify", 12288, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(motorTask,   "motor",   2048,  NULL, 2, NULL, 1);
  if (displayOk) {
    xTaskCreatePinnedToCore(displayTask, "display", 4096, NULL, 1, NULL, 1);
  }

  uint8_t v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("RC522 version now: 0x%02X%s\n", v,
                (v == 0x00 || v == 0xFF) ? "  (ERROR: reader not responding)" : "  (ok)");
  Serial.println("Ready. Put on a record and lower the tonearm.");
}

// ===========================================================================
// loop(): reads the inputs, never waits for the network
// ===========================================================================
void loop() {
  checkButtons();
  checkEncoder();
  checkTonearm();

  if (millis() - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = millis();
    checkRecord();
  }

  updatePlayback();

  // watchdog: if spotifyTask has not reported for too long it is stuck
  if (millis() - spotifyAliveMs > WATCHDOG_MS) {
    Serial.println("Spotify task stuck. Restarting the ESP32...");
    showNote("Restarting...");
    delay(1500);
    ESP.restart();
  }
  delay(2);
}
