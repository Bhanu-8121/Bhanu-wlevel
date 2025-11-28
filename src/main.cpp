/* V6 — Water Motor (Alexa + WiFiManager + OTA81 + WebLog82 + timestamps)
   - Alexa on port 80 (Espalexa)
   - OTA on port 81 (/update — kbc / 987654321)
   - Web Serial Log on port 82 (/log)
   - WiFiManager AP after 30s connect attempt (3 minute portal)
   - Manual tactile switch on D4 (GPIO2)
   - Tank safety lock: Alexa ON blocked at 100%
   - Auto ON at 0%, Auto OFF at 100%
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <Espalexa.h>

// ===== web servers & services =====
ESP8266WebServer alexaServer(80);   // Alexa /description + control (port 80)
ESP8266WebServer otaServer(81);     // OTA (port 81)
ESP8266HTTPUpdateServer httpUpdater;
ESP8266WebServer logServer(82);     // Web log (port 82)
String serialBuffer = "";

// ===== time =====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST offset 19800

// ===== LCD =====
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ===== Espalexa =====
Espalexa espalexa;

// ===== Pins =====
const int sensor1 = 14; // D5
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 4;  // D2
const int relayPin = 16; // D0
const int switchPin = 2; // D4 (user requested tactile on D4)

// ===== State =====
bool wifiOK = false;
bool apModeLaunched = false;      // ensure AP runs only once
unsigned long connectStartMillis = 0;

bool motorON = false;
unsigned long motorTime = 0;

bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;  // seconds since midnight taken from NTP at sync

String globalLevel = "0%";
int lastSwitchState = HIGH;

// blink vars for icon when initial 30s waiting
unsigned long blinkTicker = 0;
bool blinkState = false;

// custom characters for LCD
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};


// ---------------------- Logging (with timestamp) ----------------------
String nowStamp() {
  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis) / 1000;
    unsigned long total = offsetSeconds + elapsed;
    int h = (total / 3600) % 24;
    int m = (total / 60) % 60;
    int s = total % 60;
    char buf[16];
    sprintf(buf, "%02d:%02d:%02d", h, m, s);
    return String(buf);
  } else {
    // fallback: seconds since boot
    unsigned long s = millis() / 1000;
    unsigned long h = s / 3600;
    unsigned long m = (s / 60) % 60;
    unsigned long ss = s % 60;
    char buf[16];
    sprintf(buf, "%02lu:%02lu:%02lu", h, m, ss);
    return String(buf);
  }
}

void addLog(String msg) {
  String line = nowStamp() + "  " + msg;
  Serial.println(line);
  serialBuffer += line + "\n";
  if (serialBuffer.length() > 16000) serialBuffer.remove(0, 4000);
}


// ---------------------- Motor safe control ----------------------
void requestMotorOn(String source, String level) {
  if (level == "100%") {
    motorON = false;
    digitalWrite(relayPin, LOW);
    // update Alexa device brightness to 0 (device OFF)
    if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(0);
    addLog("BLOCKED: Tank full → ON rejected (" + source + ")");
    return;
  }

  motorON = true;
  digitalWrite(relayPin, HIGH);
  motorTime = millis();

  if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(255);
  addLog("Motor ON by " + source);
}

void requestMotorOff(String source) {
  motorON = false;
  digitalWrite(relayPin, LOW);
  if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(0);
  addLog("Motor OFF by " + source);
}


// ---------------------- Alexa callback (Espalexa passes brightness 0..255) ----------------------
void alexaCallback(uint8_t brightness) {
  String level = globalLevel;

  if (brightness == 0) {
    requestMotorOff("Alexa");
  } else {
    // brightness > 0 means ON request
    if (level == "100%") {
      // block ON at full tank
      motorON = false;
      digitalWrite(relayPin, LOW);
      if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(0);
      addLog("Alexa tried ON → BLOCKED (full tank)");
    } else {
      requestMotorOn("Alexa", level);
    }
  }
}


// ---------------------- Alexa server handlers ----------------------
// Espalexa provides handleAlexaApiCall(req, body).
// We'll forward both URI and request body (if any).
void alexaHandleRoot() {
  // pass URI and request body (plain) to Espalexa handler
  String body = alexaServer.hasArg("plain") ? alexaServer.arg("plain") : String();
  bool handled = espalexa.handleAlexaApiCall(alexaServer.uri(), body);
  if (!handled) {
    alexaServer.send(404, "text/plain", "Not found");
  }
}

void alexaHandleNotFound() {
  String body = alexaServer.hasArg("plain") ? alexaServer.arg("plain") : String();
  bool handled = espalexa.handleAlexaApiCall(alexaServer.uri(), body);
  if (!handled) {
    alexaServer.send(404, "text/plain", "Not found");
  }
}


// ---------------------- Web Log server ----------------------
void setupWebLogServer() {
  logServer.on("/log", HTTP_GET, []() {
    // return simple HTML with auto-refresh
    String html = "<!doctype html><html><head><meta charset='utf-8'><title>ESP Log</title>"
                  "<meta http-equiv='refresh' content='3'></head><body style='background:#000;color:#0f0;font-family:monospace'>"
                  "<pre>" + serialBuffer + "</pre></body></html>";
    logServer.send(200, "text/html", html);
  });
  logServer.begin();
  addLog("Web Serial Log ready: http://" + WiFi.localIP().toString() + ":82/log");
}


// ---------------------- OTA server ----------------------
void setupWebOTA() {
  httpUpdater.setup(&otaServer, "/update", "kbc", "987654321"); // A chosen in your last reply
  otaServer.begin();
  addLog("OTA Ready: http://" + WiFi.localIP().toString() + ":81/update (kbc / 987654321)");
}


// ---------------------- WiFiManager AP callback ----------------------
void configModeCallback(WiFiManager *wm) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Enter AP Mode");
  lcd.setCursor(0,1); lcd.print("SSID:");
  lcd.setCursor(5,1); lcd.print(wm->getConfigPortalSSID());
  addLog("Config Portal Started: " + wm->getConfigPortalSSID());
}


// ---------------------- Setup ----------------------
void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  pinMode(switchPin, INPUT_PULLUP); // D4 tactile

  // LCD
  Wire.begin(0, 5);
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(1200);
  lcd.clear();

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(1);
  lcd.setCursor(11,1); lcd.print("--:--");

  WiFi.setAutoReconnect(true);
  WiFi.begin();
  connectStartMillis = millis();

  addLog("Booting...");
  // Register Alexa device now (callback accepts brightness)
  espalexa.addDevice("Water Motor", alexaCallback);

  // set Alexa server handlers here — actual server begin when WiFi connects
  alexaServer.on("/", HTTP_GET, alexaHandleRoot);
  alexaServer.on("/", HTTP_POST, alexaHandleRoot);
  alexaServer.onNotFound(alexaHandleNotFound);
}


// ---------------------- Loop ----------------------
void loop() {
  // track connection state
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // If not connected after 30s, launch blocking config portal once
  if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {
    addLog("30s timeout. Launching Config Portal...");
    WiFiManager wm;
    wm.setAPCallback(configModeCallback);
    wm.setConfigPortalTimeout(180); // 3 minutes
    // startConfigPortal blocks and provides captive portal — returns on success (connected) or timeout.
    if (wm.startConfigPortal("KBC-Setup", "12345678")) {
      addLog("AP connection successful!");
    } else {
      addLog("AP timeout. Running offline.");
    }
    apModeLaunched = true; // don't launch again
    // re-evaluate
    isConnected = (WiFi.status() == WL_CONNECTED);
  }

  // If connected and we haven't initialized networkside servers yet
  if (isConnected && !wifiOK) {
    wifiOK = true;
    timeClient.begin();
    setupWebOTA();       // OTA on port 81
    setupWebLogServer(); // logs on 82

    // alexaServer needs to start on port 80 so Espalexa can be discovered
    alexaServer.begin();
    // Start Espalexa last (it needs the server to be reachable for discover)
    espalexa.begin();

    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(0,1); lcd.print("WiFi Connected ");
    delay(1300);
    lcd.setCursor(0,1); lcd.print("Motor:OFF ");
    lcd.setCursor(10,1); lcd.write(0);

    addLog("Alexa server started on port 80");
    addLog("WiFi Connected: " + WiFi.localIP().toString());
  }

  // handle clients on all servers (if started)
  if (isConnected) otaServer.handleClient();   // OTA
  alexaServer.handleClient();                  // Alexa endpoints (discovery/control)
  logServer.handleClient();                    // logs

  espalexa.loop(); // required

  // If we lost connection after being connected once
  if (!isConnected && wifiOK) {
    wifiOK = false;
    lcd.setCursor(0,1); lcd.print("WiFi Disconnect ");
    delay(1200);
    lcd.setCursor(0,1); lcd.print("Motor:OFF ");
    lcd.setCursor(10,1); lcd.write(1);
    addLog("WiFi Disconnected");
  }

  // time sync
  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
  }

  // current time string
  String currentTime = "--:--";
  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis) / 1000;
    unsigned long total = offsetSeconds + elapsed;
    int h = (total / 3600) % 24;
    int m = (total / 60) % 60;
    char buf[6];
    sprintf(buf, "%02d:%02d", h, m);
    currentTime = String(buf);
  }

  // ---- Read sensors (debounce aggregated) ----
  bool s1=false, s2=false, s3=false;
  int s4c = 0;
  for (int i = 0; i < 7; ++i) {
    if (digitalRead(sensor1) == LOW) s1 = true;
    if (digitalRead(sensor2) == LOW) s2 = true;
    if (digitalRead(sensor3) == LOW) s3 = true;
    if (digitalRead(sensor4) == LOW) s4c++;
    delay(6);
  }
  bool s4 = (s4c >= 5);

  // Determine level
  String level;
  if (s4 && s3 && s2 && s1) level = "100%";
  else if (s3 && s2 && s1) level = "75%";
  else if (s2 && s1) level = "50%";
  else if (s1) level = "25%";
  else level = "0%";

  globalLevel = level; // keep Alexa logic in sync

  // Update top line level display
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(12,0); lcd.print("    ");
  lcd.setCursor(12,0); lcd.print(level);

  // Auto motor logic with safety
  if (level == "0%" && !motorON) {
    requestMotorOn("System", level);
  }
  if (level == "100%" && motorON) {
    requestMotorOff("System");
  }

  // Manual tactile switch on D4 (toggle on falling edge)
  int sw = digitalRead(switchPin);
  if (lastSwitchState == HIGH && sw == LOW) {
    if (motorON) requestMotorOff("Switch");
    else requestMotorOn("Switch", level);
    delay(80); // debounce
  }
  lastSwitchState = sw;

  // Bottom line: Motor status, wifi icon, time
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[17];
    sprintf(buf, "Motor:ON %02dM  ", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);

    if (wifiOK) {
      lcd.write(0); // wifi on char
    } else {
      // if still in initial 30s window (ap not launched) → blinking indicator
      if (!apModeLaunched) {
        if (millis() - blinkTicker >= 500) {
          blinkTicker = millis();
          blinkState = !blinkState;
        }
        if (blinkState) lcd.write((uint8_t)0);
        else lcd.print(" ");
      } else {
        // AP attempted already → show wifi off char
        lcd.write(1);
      }
    }
  }

  // Time at right
  lcd.setCursor(11,1);
  lcd.print(currentTime);

  delay(200);
}
