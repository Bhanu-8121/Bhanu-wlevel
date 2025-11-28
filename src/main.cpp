/* Full corrected integrated sketch
   - WiFiManager (30s wait -> AP for 3min)
   - Alexa on port 80 (discovery + control)
   - OTA on port 81
   - Web log on port 82
   - Manual switch on D4 (GPIO2)
   - Tank-full safety lock
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

// ====== SERVERS ======
ESP8266WebServer alexaServer(80);   // MUST be port 80 for Alexa discovery
ESP8266WebServer otaServer(81);
ESP8266HTTPUpdateServer httpUpdater;
ESP8266WebServer logServer(82);

// ====== WiFiManager (global) ======
WiFiManager wm; // global so the captive portal internals remain valid

// ====== Alexa ======
Espalexa espalexa;

// ====== TIME ======
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST

// ====== LCD ======
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ====== Icons ======
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// ====== PINS (user requested D4 tactile switch) ======
const int sensor1 = 14; // D5
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 4;  // D2
const int relayPin = 16; // D0
const int switchPin = 2; // D4 (user requested) -> use INPUT_PULLUP

// ====== STATE ======
bool wifiOK = false;
bool apModeLaunched = false;
unsigned long connectStartMillis = 0;

bool motorON = false;
unsigned long motorTime = 0;

bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

String globalLevel = "0%";
int lastSwitchState = HIGH;

String serialBuffer = ""; // web log buffer

// ====== Logging helper ======
void addLog(const String &s) {
  Serial.println(s);
  serialBuffer += s + "\n";
  if (serialBuffer.length() > 12000) serialBuffer.remove(0, 3000);
}

// ====== Motor control (safe) ======
void requestMotorOn(const String &source, const String &level) {
  if (level == "100%") {
    motorON = false;
    digitalWrite(relayPin, LOW);
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

void requestMotorOff(const String &source) {
  motorON = false;
  digitalWrite(relayPin, LOW);
  if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(0);
  addLog("Motor OFF by " + source);
}

// ====== Alexa callback (Espalexa gives a brightness 0-255) ======
void alexaCallback(uint8_t brightness) {
  String level = globalLevel;
  if (brightness == 0) {
    requestMotorOff("Alexa");
  } else {
    // treat any non-zero as ON request (Espalexa sends brightness)
    if (level == "100%") {
      motorON = false;
      digitalWrite(relayPin, LOW);
      if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(0);
      addLog("Alexa tried ON → BLOCKED (full tank)");
    } else {
      requestMotorOn("Alexa", level);
    }
  }
}

// ====== Alexa server HTTP handlers ======
void alexaRootHandler() {
  // pass URI and optional state arg to Espalexa
  // handleAlexaApiCall(req, body) signature: we pass uri and state
  if (!espalexa.handleAlexaApiCall(alexaServer.uri(), alexaServer.arg("state"))) {
    alexaServer.send(200, "text/plain", "");
  }
}
void alexaNotFoundHandler() {
  if (!espalexa.handleAlexaApiCall(alexaServer.uri(), alexaServer.arg("state"))) {
    alexaServer.send(404, "text/plain", "Not found");
  }
}

// ====== WiFiManager callbacks ======
void configModeCallback(WiFiManager *myWM) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Enter AP Mode");
  lcd.setCursor(0, 1); lcd.print("SSID:");
  lcd.setCursor(5, 1); lcd.print(myWM->getConfigPortalSSID());
  addLog("Config Portal: " + myWM->getConfigPortalSSID());
}

// ====== OTA & Log servers setup ======
void setupWebOTA() {
  httpUpdater.setup(&otaServer, "/update", "kbc", "987654321");
  otaServer.begin();
  addLog("OTA Ready: http://" + WiFi.localIP().toString() + ":81/update");
}

void setupWebLogServer() {
  logServer.on("/log", HTTP_GET, []() {
    logServer.send(200, "text/plain", serialBuffer);
  });
  logServer.begin();
  addLog("Web Log ready: http://" + WiFi.localIP().toString() + ":82/log");
}

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  delay(50);

  // pins
  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  pinMode(switchPin, INPUT_PULLUP);

  // LCD
  Wire.begin(0, 5);
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);
  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(1500);
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(1);
  lcd.setCursor(11,1); lcd.print("--:--");

  // WiFi begin non-blocking
  WiFi.setAutoReconnect(true);
  WiFi.begin();
  connectStartMillis = millis();

  // WiFiManager initial config (global instance)
  wm.setAPCallback(configModeCallback);
  // don't call startConfigPortal here - we wait in loop to allow 30s initial connect window

  // Alexa: register device (do NOT call espalexa.begin() yet — we will start Alexa server after WiFi connect)
  espalexa.addDevice("Water Motor", alexaCallback);

  addLog("Booting...");
}

// ====== Main Loop ======
void loop() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // --- WiFi/AP Manager Logic: wait 30s for WiFi to connect, then launch AP (only once) ---
  if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {
    addLog("30s timeout. Launching Config Portal...");
    // This call blocks with the captive portal UI for up to timeout set below.
    // We set a 180s timeout on startConfigPortal below.
    wm.setConfigPortalTimeout(180); // 3 minutes
    if (wm.startConfigPortal("KBC-Setup", "12345678")) {
      addLog("AP connection successful!");
    } else {
      addLog("AP timeout or canceled - running offline.");
    }
    apModeLaunched = true; // ensure we don't try again automatically
    // re-check connectivity
    isConnected = (WiFi.status() == WL_CONNECTED);
  }

  // If connected and not yet configured servers, configure them once
  if (isConnected && !wifiOK) {
    wifiOK = true;
    addLog("WiFi Connected: " + WiFi.localIP().toString());

    // Start servers that require WiFi
    setupWebOTA();
    setupWebLogServer();

    // Start Alexa HTTP server (port 80) and attach handlers
    alexaServer.on("/", HTTP_GET, alexaRootHandler);
    alexaServer.onNotFound(alexaNotFoundHandler);
    alexaServer.begin();
    // Start Espalexa after web server present
    espalexa.begin();
    addLog("Alexa server started on port 80 and Espalexa begun");

    // NTP
    timeClient.begin();

    // small LCD update
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(0,1); lcd.print("WiFi Connected");
    delay(1200);
  }

  // handle server clients (call every loop)
  if (isConnected) {
    otaServer.handleClient();
    alexaServer.handleClient();
  }
  logServer.handleClient();
  espalexa.loop();

  // detect disconnect
  if (!isConnected && wifiOK) {
    wifiOK = false;
    addLog("WiFi Disconnected");
    lcd.setCursor(0,1); lcd.print("WiFi Disconnect ");
    delay(1000);
    lcd.setCursor(0,1); lcd.print("Motor:OFF ");
    lcd.setCursor(10,1); lcd.write(1);
  }

  // NTP/time
  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
  }
  String timeStr = "--:--";
  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis)/1000;
    unsigned long total = offsetSeconds + elapsed;
    int h = (total / 3600) % 24;
    int m = (total / 60) % 60;
    char buf[6];
    sprintf(buf, "%02d:%02d", h, m);
    timeStr = buf;
  }

  // ===== Read sensors (debounce-ish) =====
  bool s1=false,s2=false,s3=false;
  int s4c=0;
  for (int i=0;i<7;i++){
    if (digitalRead(sensor1)==LOW) s1=true;
    if (digitalRead(sensor2)==LOW) s2=true;
    if (digitalRead(sensor3)==LOW) s3=true;
    if (digitalRead(sensor4)==LOW) s4c++;
    delay(10);
  }
  bool s4 = (s4c >= 5);

  String level;
  if (s4 && s3 && s2 && s1) level = "100%";
  else if (s3 && s2 && s1) level = "75%";
  else if (s2 && s1) level = "50%";
  else if (s1) level = "25%";
  else level = "0%";

  globalLevel = level; // keep Alexa logic in sync

  // update top-line LCD
  lcd.setCursor(0,0);
  lcd.print("Water Level:");
  lcd.setCursor(12,0);
  lcd.print("    ");
  lcd.setCursor(12,0);
  lcd.print(level);

  // Auto motor logic
  if (level == "0%" && !motorON) requestMotorOn("System", level);
  if (level == "100%" && motorON) requestMotorOff("System");

  // Manual switch (toggle on falling edge)
  int sw = digitalRead(switchPin);
  if (lastSwitchState == HIGH && sw == LOW) {
    if (motorON) requestMotorOff("Switch");
    else requestMotorOn("Switch", level);
    delay(80); // debounce
  }
  lastSwitchState = sw;

  // bottom line: motor status, wifi icon, time
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[17];
    sprintf(buf, "Motor:ON %02dM  ", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);
    // wifi icon
    if (wifiOK) lcd.write(0);
    else {
      // if AP hasn't been attempted yet, blink icon while in initial 30s
      if (!apModeLaunched) {
        static unsigned long blinkTicker = 0;
        static bool blinkState = false;
        if (millis() - blinkTicker >= 500) { blinkTicker = millis(); blinkState = !blinkState; }
        if (blinkState) lcd.write((uint8_t)0);
        else lcd.print(" ");
      } else {
        lcd.write(1);
      }
    }
    lcd.setCursor(11,1); lcd.print(timeStr);
  }

  // small loop delay
  delay(180);
}
