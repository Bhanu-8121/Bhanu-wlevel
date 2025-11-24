/* Final merged: WiFiManager (30s AP) + Alexa (legacy Espalexa)
   - Alexa on port 80 (legacy, discovery)
   - OTA on port 81
   - Web log on port 82
   - WiFiManager: try saved wifi, if not connected within 30s -> AP portal
   - No AP after successful connect (reconnect only)
   - Alexa brightness callback (0..255)
   - Manual switch (D4 / GPIO2) to GND with INPUT_PULLUP
   - Sensors and motor auto logic preserved
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

// === Servers & Alexa ===
ESP8266WebServer alexaServer(80);
Espalexa espalexa;

// OTA port 81
ESP8266WebServer otaServer(81);
ESP8266HTTPUpdateServer httpUpdater;

// Web log port 82
ESP8266WebServer logServer(82);
String serialBuffer = "";

// === Logging helper ===
void addLog(const String &s) {
  Serial.println(s);
  serialBuffer += s + "\n";
  if (serialBuffer.length() > 12000) serialBuffer.remove(0, 4000);
}

// === Time ===
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

// === LCD ===
LiquidCrystal_I2C lcd(0x27, 16, 2);
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// === Pins ===
const int sensor1 = 14; // D5
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 4;  // D2
const int relayPin = 16; // D0
const int switchPin = 2; // D4

// === States ===
bool wifiOK = false;
bool apModeLaunched = false;
unsigned long connectStartMillis = 0;

bool motorON = false;
unsigned long motorTime = 0;

String globalLevel = "0%";
int lastSwitchState = HIGH;

// === Forward declarations ===
void requestMotorOn(const String &source, const String &level);
void requestMotorOff(const String &source);
void alexaCallback(uint8_t brightness);
void setupAlexa();
void alexaServerSetup();
void setupWebOTA();
void setupWebLogServer();
void configModeCallback(WiFiManager *wm);

// ------------------ Motor control ------------------
void requestMotorOn(const String &source, const String &level) {
  if (level == "100%") {
    motorON = false;
    digitalWrite(relayPin, LOW);
    if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(0);
    addLog("BLOCKED: Tank full -> ON rejected (" + source + ")");
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

// ------------------ Alexa callback (brightness) ------------------
void alexaCallback(uint8_t brightness) {
  String level = globalLevel;
  if (brightness == 0) {
    requestMotorOff("Alexa");
  } else {
    if (level == "100%") {
      // block ON
      motorON = false;
      digitalWrite(relayPin, LOW);
      if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(0);
      addLog("Alexa tried ON -> BLOCKED (tank full)");
    } else {
      requestMotorOn("Alexa", level);
    }
  }
}

// ------------------ Setup Alexa (register) ------------------
void setupAlexa() {
  espalexa.addDevice("Water Motor", alexaCallback);
  addLog("Espalexa device registered: Water Motor");
}

// ------------------ Alexa server wiring (legacy) ------------------
void alexaServerSetup() {
  // forward all unknown requests to Espalexa
  alexaServer.onNotFound([]() {
    String req = alexaServer.uri();
    String body = alexaServer.arg("state");
    if (!espalexa.handleAlexaApiCall(req, body)) {
      alexaServer.send(404, "text/plain", "Not Found");
    }
  });
  alexaServer.begin();
  addLog("Alexa HTTP server started on port 80");
}

// ------------------ OTA server ------------------
void setupWebOTA() {
  httpUpdater.setup(&otaServer, "/update", "kbc", "987654321");
  otaServer.begin();
  addLog("OTA Ready: http://" + WiFi.localIP().toString() + ":81/update");
}

// ------------------ Web log server ------------------
void setupWebLogServer() {
  logServer.on("/log", HTTP_GET, []() {
    logServer.send(200, "text/plain", serialBuffer);
  });
  logServer.begin();
  addLog("Web Log Ready: http://" + WiFi.localIP().toString() + ":82/log");
}

// ------------------ WiFiManager AP callback ------------------
void configModeCallback(WiFiManager *wm) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Enter AP Mode");
  lcd.setCursor(0,1); lcd.print("SSID:");
  lcd.setCursor(5,1); lcd.print(wm->getConfigPortalSSID());
  addLog("Entered config mode: " + wm->getConfigPortalSSID());
}

// ------------------ Setup ------------------
void setup() {
  Serial.begin(115200);
  addLog("Booting...");

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(switchPin, INPUT_PULLUP);
  digitalWrite(relayPin, LOW);

  Wire.begin(0,5);
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

  WiFi.setAutoReconnect(true);
  WiFi.begin();
  connectStartMillis = millis();

  // register Alexa device (do not begin yet until server available)
  setupAlexa();
}

// ------------------ Loop ------------------
void loop() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // AP fallback after 30s if not connected
  if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {
    addLog("30s timeout, launching config portal...");
    WiFiManager wm;
    wm.setAPCallback(configModeCallback);
    wm.setConfigPortalTimeout(180);
    if (wm.startConfigPortal("KBC-Setup", "12345678")) {
      addLog("Config portal finished, should be connected now");
    } else {
      addLog("Config portal timeout or closed");
    }
    apModeLaunched = true;
    isConnected = (WiFi.status() == WL_CONNECTED);
  }

  // When WiFi connects first time: start servers (OTA,log) and Alexa server, attach Espalexa
  if (isConnected && !wifiOK) {
    wifiOK = true;

    timeClient.begin();
    setupWebOTA();
    setupWebLogServer();

    // Start Alexa HTTP server and attach Espalexa to it (legacy attach)
    alexaServerSetup();
    // Some Espalexa versions support begin(&server) — it's safe to call with alexaServer
    espalexa.begin(&alexaServer);
    addLog("Espalexa begun and attached to alexaServer");

    addLog("WiFi Connected: " + WiFi.localIP().toString());
    lcd.clear();
    lcd.setCursor(0,1); lcd.print("WiFi Connected");
    delay(1000);
  }

  // Handle servers and Espalexa loop
  if (isConnected) otaServer.handleClient();
  alexaServer.handleClient();
  logServer.handleClient();
  espalexa.loop();

  // Time sync
  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getSeconds() + timeClient.getMinutes()*60 + timeClient.getHours()*3600;
  }

  String timeStr = "--:--";
  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis) / 1000;
    unsigned long total = offsetSeconds + elapsed;
    int h = (total / 3600) % 24;
    int m = (total / 60) % 60;
    char buf[6];
    sprintf(buf, "%02d:%02d", h, m);
    timeStr = buf;
  }

  // Read sensors (debounce)
  bool s1=false, s2=false, s3=false;
  int s4c=0;
  for (int i=0; i<7; i++) {
    if (digitalRead(sensor1) == LOW) s1 = true;
    if (digitalRead(sensor2) == LOW) s2 = true;
    if (digitalRead(sensor3) == LOW) s3 = true;
    if (digitalRead(sensor4) == LOW) s4c++;
    delay(10);
  }
  bool s4 = (s4c >= 5);

  String level;
  if (s4 && s3 && s2 && s1) level = "100%";
  else if (s3 && s2 && s1) level = "75%";
  else if (s2 && s1) level = "50%";
  else if (s1) level = "25%";
  else level = "0%";

  globalLevel = level;

  lcd.setCursor(12,0);
  lcd.print(level + " ");

  // Auto motor logic
  if (level == "0%" && !motorON) requestMotorOn("System", level);
  if (level == "100%" && motorON) requestMotorOff("System");

  // Manual switch (D4 to GND) debounced simple press detection
  int sw = digitalRead(switchPin);
  if (lastSwitchState == HIGH && sw == LOW) {
    if (motorON) requestMotorOff("Switch");
    else requestMotorOn("Switch", level);
    delay(80);
  }
  lastSwitchState = sw;

  // LCD bottom row
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[17];
    sprintf(buf, "Motor:ON %02dM", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);
    lcd.write(wifiOK ? 0 : 1);
  }

  lcd.setCursor(11,1);
  lcd.print(timeStr);

  delay(200);
}
