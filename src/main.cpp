/* Final corrected KBC Water Level Controller
   - Option 1 behavior after AP timeout (normal screen with wifiOff)
   - Fixed compile errors: removed removeHandler(), fixed fauxmo.enable()
   - Blink rules: trySaved=1s, AP=0.5s, no-wifi=wifiOff
   - WiFi persist + ESP.restart() on successful config
   - WiFi icon at line2 col10; time at line2 col11..15 (HH:MM) shown only when motor OFF
   - All pins unchanged; WebOTA & fauxmo preserved
*/

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <fauxmoESP.h>

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
fauxmoESP fauxmo;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST

LiquidCrystal_I2C lcd(0x27, 16, 2);

// Pins (unchanged)
const int sensor1 = 14;  // D5
const int sensor2 = 12;  // D6
const int sensor3 = 13;  // D7
const int sensor4 = 4;   // D2
const int relayPin = 16; // D0
const int manualPin = 2; // D4

// WiFi symbols (as provided)
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// State variables
bool wifiConnected = false;   // when STA connected
bool apActive = false;        // AP server running
unsigned long apStartMillis = 0;

bool motorON = false;
bool lastMotorState = false;
unsigned long motorTime = 0;

// Soft-RTC
bool timeSynced = false;
unsigned long rtcOffsetSeconds = 0;
unsigned long rtcLastSyncMillis = 0;

// Timers & intervals
const unsigned long TRY_SAVED_MS   = 15000UL;   // try saved wifi for 15s
const unsigned long AP_TOTAL_MS    = 120000UL;  // AP stays for 2 minutes
const unsigned long AP_SCROLL_MS   = 30000UL;   // start scrolling after 30s

// Blink control
unsigned long blinkTicker = 0;
bool blinkState = false;

// Helper: format HH:MM from seconds
void formatHHMM(unsigned long totalSeconds, char *out) {
  unsigned long h = (totalSeconds / 3600UL) % 24;
  unsigned long m = (totalSeconds / 60UL) % 60;
  sprintf(out, "%02lu:%02lu", h, m);
}

// WebOTA setup
void setupWebOTA() {
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
  Serial.println("WebOTA enabled at /update (kbc / 987654321)");
}

// fauxmo callback
void wemoCallback(unsigned char device_id, const char * device_name, bool state, unsigned char value) {
  Serial.printf("fauxmo: %s -> %s\n", device_name, state ? "ON" : "OFF");
  motorON = state;
  if (motorON && !lastMotorState) motorTime = millis();
}

// LCD helper: show wifi connected icon at line2 col10 (zero-based)
void lcdShowWifiOn()  { lcd.setCursor(10,1); lcd.write((uint8_t)0); }
void lcdShowWifiOff() { lcd.setCursor(10,1); lcd.write((uint8_t)1); }
void lcdClearWifiIcon(){ lcd.setCursor(10,1); lcd.print(" "); }

// Blink with custom interval (ms). We'll call with either 1000 or 500 ms.
void lcdBlinkWifi(unsigned long intervalMs) {
  unsigned long now = millis();
  if (now - blinkTicker >= intervalMs) {
    blinkTicker = now;
    blinkState = !blinkState;
    lcd.setCursor(10,1);
    if (blinkState) lcd.write((uint8_t)0); // show wifiOn char
    else lcd.print(" ");
  }
}

// -----------------------------
// Simple non-blocking AP config
// -----------------------------
// HTML page & handlers
String htmlFormPage(const String &msg = "") {
  String s = "<!doctype html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'/><title>KBC Setup</title></head><body>";
  s += "<h3>KBC-Setup</h3>";
  if (msg.length()) s += "<p><b>" + msg + "</b></p>";
  s += "<form method='POST' action='/save'>";
  s += "SSID:<br><input name='ssid' placeholder='Your WiFi'><br>Password:<br><input name='pwd' type='password'><br><br>";
  s += "<input type='submit' value='Save & Connect'></form>";
  s += "<p>AP: KBC-Setup (pwd 12345678)</p>";
  s += "</body></html>";
  return s;
}

void handleRoot() {
  server.send(200, "text/html", htmlFormPage());
}

// When /save called, attempt connection and respond
void handleSave() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "Missing SSID");
    return;
  }
  String ssid = server.arg("ssid");
  String pwd  = server.arg("pwd");
  Serial.printf("Received config: SSID='%s' PWD='%s'\n", ssid.c_str(), pwd.c_str());

  // Persist credentials (ensure persistence)
  WiFi.persistent(true);

  // Begin connecting (non-blocking - we'll check in loop)
  WiFi.begin(ssid.c_str(), pwd.c_str());

  String resp = "<!doctype html><html><body><h3>Attempting to connect...</h3><p>If successful the device will stop AP and reboot to apply settings.</p></body></html>";
  server.send(200, "text/html", resp);
}

// Start AP and register handlers (non-blocking)
void startCustomAP() {
  if (apActive) return;
  Serial.println("Starting AP: KBC-Setup");
  WiFi.mode(WIFI_AP_STA);            // allow STA + AP
  WiFi.softAP("KBC-Setup", "12345678");
  delay(200);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP: "); Serial.println(apIP);

  // register HTTP handlers
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  apActive = true;
  apStartMillis = millis();
  blinkTicker = millis();
  blinkState = false;
}

// Stop AP (DO NOT call removeHandler)
void stopCustomAP() {
  if (!apActive) return;
  Serial.println("Stopping AP");
  server.stop();                  // stop listening; handlers may remain but server is stopped
  WiFi.softAPdisconnect(true);
  apActive = false;
}

// Try saved WiFi for TRY_SAVED_MS while blinking every 1s. Returns true if connected.
bool trySavedWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // attempt using stored credentials
  unsigned long start = millis();
  blinkTicker = millis();
  blinkState = false;

  // show initial info on LCD
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(11,1); lcd.print("--:--"); // placeholder time

  while (millis() - start < TRY_SAVED_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      Serial.println("Saved WiFi connected");
      // NTP init & initial sync
      timeClient.begin();
      if (timeClient.update()) {
        timeSynced = true;
        rtcOffsetSeconds = (unsigned long)timeClient.getHours()*3600UL + (unsigned long)timeClient.getMinutes()*60UL + (unsigned long)timeClient.getSeconds();
        rtcLastSyncMillis = millis();
      }
      return true;
    }
    // blink every 1s while trying saved wifi
    lcdBlinkWifi(1000UL);
    delay(100);
  }
  // timeout
  Serial.println("Saved WiFi not found within timeout");
  WiFi.disconnect(true);
  wifiConnected = false;
  return false;
}

// get current HH:MM (from NTP if online and synced; else soft-RTC if have sync)
void getCurrentHHMM(char *out) {
  if (timeSynced && WiFi.status() == WL_CONNECTED) {
    String t = timeClient.getFormattedTime();
    String hhmm = t.substring(0,5);
    hhmm.toCharArray(out, 6);
    return;
  } else if (timeSynced) {
    unsigned long elapsed = (millis() - rtcLastSyncMillis) / 1000UL;
    unsigned long total = rtcOffsetSeconds + elapsed;
    formatHHMM(total, out);
    return;
  } else {
    strcpy(out, "--:--");
    return;
  }
}

// Setup
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nBooting KBC Controller...");

  // pins
  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(manualPin, INPUT_PULLUP);
  digitalWrite(relayPin, LOW);

  // LCD init
  Wire.begin(0,5);
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  // splash (old)
  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000);
  lcd.clear();

  // initial display (water-level placeholder)
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write((uint8_t)1);
  lcd.setCursor(11,1); lcd.print("--:--");

  // 1) try saved WiFi with 1s blink
  if (trySavedWifi()) {
    Serial.print("Connected (IP): "); Serial.println(WiFi.localIP());
  } else {
    // 2) not found -> start non-blocking AP server; LCD will continue to update in loop
    startCustomAP();
  }

  // setup OTA & fauxmo
  setupWebOTA();
  fauxmo.createServer(true);
  fauxmo.setPort(80);
  fauxmo.addDevice("Motor");
  fauxmo.onSetState(wemoCallback);
  fauxmo.enable(true);   // <-- fixed API: single argument

  Serial.println("Setup complete.");
}

// main loop
void loop() {
  // handle incoming web and OTA requests
  server.handleClient();
  fauxmo.handle();

  // If AP active => handle AP UI (blink 0.5s & scrolling after 30s)
  if (apActive) {
    unsigned long now = millis();
    // blink every 0.5s in AP mode
    if (now - blinkTicker >= 500UL) {
      blinkTicker = now;
      blinkState = !blinkState;
      lcd.setCursor(10,1);
      if (blinkState) lcd.write((uint8_t)0);
      else lcd.print(" ");
    }

    // scrolling message after 30s
    if (now - apStartMillis >= AP_SCROLL_MS) {
      static unsigned long lastScroll = 0;
      static int offset = 0;
      String msg = "Connect to KBC-Setup and select your WiFi";
      int len = msg.length();
      if (now - lastScroll >= 300) {
        lastScroll = now;
        String part = "";
        for (int i = 0; i < 16; ++i) {
          int idx = offset + i;
          if (idx < len) part += msg[idx];
          else part += ' ';
        }
        lcd.setCursor(0,1);
        lcd.print(part);
        offset++;
        if (offset > len) offset = 0;
      }
    }

    // If user submitted credentials, WiFi.begin was called by handleSave()
    // check for connection success
    if (WiFi.status() == WL_CONNECTED) {
      // connected via AP config -> save & reboot
      wifiConnected = true;
      Serial.println("Connected after AP config - saving and rebooting...");
      // Ensure persistent storage of credentials
      WiFi.persistent(true);
      // small delay to stabilize
      delay(500);
      // stop AP and server before restart
      stopCustomAP();
      // short delay then restart to apply
      delay(300);
      ESP.restart();
      return; // restart imminent
    }

    // AP total timeout
    if (millis() - apStartMillis >= AP_TOTAL_MS) {
      Serial.println("AP timeout expired (no config). Stopping AP.");
      stopCustomAP();
      apActive = false;
      wifiConnected = false;

      // Option 1: show normal screen (wifiOff) after AP timeout
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("Water Level:");
      lcd.setCursor(0,1); lcd.print("Motor:OFF ");
      lcd.setCursor(10,1); lcd.write((uint8_t)1);
      lcd.setCursor(11,1); lcd.print("--:--");
      delay(800);
      // continue to normal offline behavior
    }

    // while AP is active we skip the rest of normal processing in this iteration
    return;
  } // end apActive handling

  // Normal mode (connected or offline)
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  if (isConnected) {
    // update NTP when available (timeClient manages its own interval)
    if (timeClient.update()) {
      timeSynced = true;
      rtcOffsetSeconds = (unsigned long)timeClient.getHours()*3600UL + (unsigned long)timeClient.getMinutes()*60UL + (unsigned long)timeClient.getSeconds();
      rtcLastSyncMillis = millis();
    }
  }

  // manual button toggle (simple debounce)
  static unsigned long lastPress = 0;
  if (digitalRead(manualPin) == LOW) {
    if (millis() - lastPress > 300) {
      motorON = !motorON;
      Serial.println("Manual toggle -> motorON = " + String(motorON));
      if (motorON) motorTime = millis();
      lastPress = millis();
      delay(200);
    }
  }

  // read sensors (quick)
  bool s1=false,s2=false,s3=false,s4=false;
  int s4count = 0;
  for (int i=0;i<7;i++){
    if (digitalRead(sensor1) == LOW) s1 = true;
    if (digitalRead(sensor2) == LOW) s2 = true;
    if (digitalRead(sensor3) == LOW) s3 = true;
    if (digitalRead(sensor4) == LOW) s4count++;
    delay(1);
  }
  s4 = (s4count >= 5);

  String level;
  if (s4 && s3 && s2 && s1) level = "100%";
  else if (s3 && s2 && s1) level = "75%";
  else if (s2 && s1) level = "50%";
  else if (s1) level = "25%";
  else level = "0%";

  // safety overrides
  if (level == "0%") motorON = true;
  if (level == "100%") motorON = false;

  // motor/relay
  if (motorON && !lastMotorState) motorTime = millis();
  lastMotorState = motorON;
  digitalWrite(relayPin, motorON ? HIGH : LOW);

  // LCD line 1: Water Level
  lcd.setCursor(0,0);
  lcd.print("Water Level:");
  lcd.setCursor(12,0);
  lcd.print("    ");
  lcd.setCursor(12,0);
  lcd.print(level);

  // LCD line 2
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[17];
    sprintf(buf, "Motor:ON frm %02dM", mins);
    lcd.print(buf); // this prints up to col 15
    // wifi icon at col10
    if (isConnected) lcd.setCursor(10,1), lcd.write((uint8_t)0);
    else lcd.setCursor(10,1), lcd.write((uint8_t)1);
  } else {
    // motor OFF -> show HH:MM at cols 11..15 (lcd.setCursor(11,1))
    char hhmm[6];
    getCurrentHHMM(hhmm);
    lcd.print("Motor:OFF ");
    lcd.setCursor(11,1);
    lcd.print(hhmm); // occupies 5 chars
    // wifi icon at col10 (left of time)
    if (isConnected) lcd.setCursor(10,1), lcd.write((uint8_t)0);
    else lcd.setCursor(10,1), lcd.write((uint8_t)1);
  }

  delay(200); // modest UI update rate to avoid flicker
}
