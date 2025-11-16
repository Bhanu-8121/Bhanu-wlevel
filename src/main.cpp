/* Final KBC Water Level Controller (NTP reconnect fix)
   - Added lastConnected detection and NTP resync on reconnect
   - Reconnect retry interval set to 30s
   - If user configures WiFi in AP mode and device connects, it persists and restarts
   - All LCD positions and WiFi icon behavior unchanged
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

// Pins
const int sensor1 = 14;  // D5
const int sensor2 = 12;  // D6
const int sensor3 = 13;  // D7
const int sensor4 = 4;   // D2
const int relayPin = 16; // D0
const int manualPin = 2; // D4

// WiFi icons
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Flags & timers
bool wifiConnected = false;
bool apActive = false;
unsigned long apStartMillis = 0;
unsigned long blinkTicker = 0;
bool blinkState = false;

bool motorON = false;
bool lastMotorState = false;
unsigned long motorTime = 0;

bool timeSynced = false;
unsigned long rtcOffsetSeconds = 0;
unsigned long rtcLastSyncMillis = 0;

bool lastConnected = false; // <-- NEW: track last connection state

const unsigned long TRY_SAVED_MS   = 15000UL;
const unsigned long AP_TOTAL_MS    = 120000UL;
const unsigned long AP_SCROLL_MS   = 30000UL;

// Forward declarations
void startCustomAP();
void stopCustomAP();

// ----------------------------------------------------
// Utility: format HH:MM
// ----------------------------------------------------
void formatHHMM(unsigned long totalSeconds, char *out) {
  unsigned long h = (totalSeconds / 3600UL) % 24;
  unsigned long m = (totalSeconds / 60UL) % 60;
  sprintf(out, "%02lu:%02lu", h, m);
}

// ----------------------------------------------------
// Web OTA
// ----------------------------------------------------
void setupWebOTA() {
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
  Serial.println("WebOTA enabled");
}

// ----------------------------------------------------
// Fauxmo callback
// ----------------------------------------------------
void wemoCallback(unsigned char device_id, const char *device_name, bool state, unsigned char value) {
  motorON = state;
  if (motorON && !lastMotorState) motorTime = millis();
}

// ----------------------------------------------------
// LCD ICON HELPERS
// ----------------------------------------------------
void lcdShowWifi(bool connected) {
  lcd.setCursor(10,1);
  lcd.write((uint8_t)(connected ? 0 : 1));
}

// ----------------------------------------------------
// HTML for AP config
// ----------------------------------------------------
String htmlFormPage(const String &msg = "") {
  String s = "<!doctype html><html><body>";
  s += "<h3>KBC-Setup</h3>";
  if (msg.length()) s += "<p><b>" + msg + "</b></p>";
  s += "<form method='POST' action='/save'>";
  s += "SSID:<br><input name='ssid'><br>Password:<br><input type='password' name='pwd'><br><br>";
  s += "<input type='submit' value='Save & Connect'></form>";
  s += "<p>AP: KBC-Setup / 12345678</p>";
  s += "</body></html>";
  return s;
}

void handleRoot() {
  server.send(200, "text/html", htmlFormPage());
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pwd  = server.arg("pwd");

  // Persist credentials flag; WiFi.begin will attempt to connect
  WiFi.persistent(true);
  WiFi.begin(ssid.c_str(), pwd.c_str());

  server.send(200, "text/html",
    "<html><body><p>Connecting... device will reboot if successful.</p></body></html>"
  );
}

// ----------------------------------------------------
// Start AP mode
// ----------------------------------------------------
void startCustomAP() {
  if (apActive) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("KBC-Setup", "12345678");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  apActive = true;
  apStartMillis = millis();
  blinkTicker = millis();

  Serial.println("AP MODE STARTED");
}

// ----------------------------------------------------
// Stop AP
// ----------------------------------------------------
void stopCustomAP() {
  if (!apActive) return;
  server.stop();
  WiFi.softAPdisconnect(true);
  apActive = false;
  Serial.println("AP stopped");
}

// ----------------------------------------------------
// Try saved WiFi (15 sec) - now also initializes NTP on success
// ----------------------------------------------------
bool trySavedWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin();

  unsigned long start = millis();

  while (millis() - start < TRY_SAVED_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;

      // Initialize NTP on first connection attempt success
      timeClient.begin();
      if (timeClient.update()) {
        timeSynced = true;
        rtcOffsetSeconds =
          (unsigned long)timeClient.getHours()*3600UL +
          (unsigned long)timeClient.getMinutes()*60UL +
          (unsigned long)timeClient.getSeconds();
        rtcLastSyncMillis = millis();
        Serial.println("Initial NTP sync OK");
      } else {
        Serial.println("Initial NTP update failed (will retry on reconnect)");
      }

      lastConnected = true;
      return true;
    }
    delay(200);
  }

  wifiConnected = false;
  lastConnected = false;
  return false;
}

// ----------------------------------------------------
// Soft RTC time retrieval
// ----------------------------------------------------
void getCurrentHHMM(char *out) {
  if (timeSynced && WiFi.status() == WL_CONNECTED) {
    String t = timeClient.getFormattedTime();
    t.substring(0,5).toCharArray(out, 6);
    return;
  }
  if (timeSynced) {
    unsigned long elapsed = (millis() - rtcLastSyncMillis)/1000UL;
    unsigned long total = rtcOffsetSeconds + elapsed;
    formatHHMM(total, out);
    return;
  }
  strcpy(out, "--:--");
}

// ----------------------------------------------------
// SETUP
// ----------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(manualPin, INPUT_PULLUP);

  Wire.begin(0,5);
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.clear();
  lcd.setCursor(4,0); lcd.print("KBC");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(1500);

  // Try saved WiFi and initialize NTP if successful
  if (!trySavedWifi()) {
    startCustomAP();
  }

  setupWebOTA();

  // Fauxmo fix
  fauxmo.createServer(true);
  fauxmo.setPort(80);
  fauxmo.addDevice("Motor");
  fauxmo.onSetState(wemoCallback);
  fauxmo.enable(true);     // FIXED
}

// ----------------------------------------------------
// LOOP
// ----------------------------------------------------
void loop() {

  server.handleClient();
  fauxmo.handle();

  // --------------------------------------------------
  // Read sensors (works in AP mode & normal mode)
  // --------------------------------------------------
  bool s1 = digitalRead(sensor1) == LOW;
  bool s2 = digitalRead(sensor2) == LOW;
  bool s3 = digitalRead(sensor3) == LOW;
  bool s4 = digitalRead(sensor4) == LOW;

  String level = "0%";
  if (s4 && s3 && s2 && s1) level = "100%";
  else if (s3 && s2 && s1)  level = "75%";
  else if (s2 && s1)        level = "50%";
  else if (s1)              level = "25%";

  // AP MODE -----------------------------------------------------
  if (apActive) {

    // Display actual % in AP mode also
    lcd.setCursor(0,0);
    lcd.print("Water Level:");
    lcd.setCursor(12,0);
    lcd.print("    ");
    lcd.setCursor(12,0);
    lcd.print(level);

    // Blink WiFi icon (0.5s) while in AP
    if (millis() - blinkTicker >= 500) {
      blinkTicker = millis();
      blinkState = !blinkState;
      lcd.setCursor(10,1);
      if (blinkState) lcd.write((uint8_t)0);
      else lcd.print(" ");
    }

    // If user-submitted credentials resulted in connection, persist + restart
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected after AP config - persisting & restarting...");
      WiFi.persistent(true);
      stopCustomAP();
      delay(300);
      ESP.restart();
      return;
    }

    // AP timeout -> stop AP and continue offline
    if (millis() - apStartMillis > AP_TOTAL_MS) {
      stopCustomAP();
      // show normal offline screen briefly
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("Water Level:");
      lcd.setCursor(12,0); lcd.print(level);
      lcd.setCursor(0,1); lcd.print("Motor:OFF ");
      lcd.setCursor(10,1); lcd.write((uint8_t)1);
      lcd.setCursor(11,1); lcd.print("--:--");
      delay(800);
    }

    return; // stop normal processing while AP active
  }

  // NORMAL MODE -------------------------------------------------

  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // -------- Auto reconnect every 30 seconds ------------
  static unsigned long lastReconnectTry = 0;
  if (!isConnected) {
    if (millis() - lastReconnectTry >= 30000UL) {
      lastReconnectTry = millis();
      Serial.println("Attempting background reconnect to saved WiFi...");
      WiFi.begin(); // retry saved credentials
    }
  }

  // -------- Detect fresh reconnect and re-init NTP ----------
  if (isConnected && !lastConnected) {
    Serial.println("WiFi just reconnected -> re-init NTP");
    timeClient.begin();
    if (timeClient.update()) {
      timeSynced = true;
      rtcOffsetSeconds =
        (unsigned long)timeClient.getHours()*3600UL +
        (unsigned long)timeClient.getMinutes()*60UL +
        (unsigned long)timeClient.getSeconds();
      rtcLastSyncMillis = millis();
      Serial.println("NTP sync successful on reconnect");
    } else {
      Serial.println("NTP update failed on reconnect (will try later)");
    }
    lastConnected = true;
  }

  if (!isConnected) lastConnected = false;

  // If connected, keep NTP updated (timeClient manages interval)
  if (isConnected) {
    if (timeClient.update()) {
      // update soft-RTC baseline when a fresh update happens
      timeSynced = true;
      rtcOffsetSeconds =
        (unsigned long)timeClient.getHours()*3600UL +
        (unsigned long)timeClient.getMinutes()*60UL +
        (unsigned long)timeClient.getSeconds();
      rtcLastSyncMillis = millis();
    }
  }

  // Manual toggle (debounce simplistic)
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

  // Motor control safety overrides
  if (level == "0%") motorON = true;
  if (level == "100%") motorON = false;

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
    sprintf(buf, "Motor:ON %02dM  ", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    char hhmm[6];
    getCurrentHHMM(hhmm);
    lcd.setCursor(11,1);
    lcd.print(hhmm);
  }

  // WiFi icon at col10
  lcdShowWifi(isConnected);

  delay(200);
}
