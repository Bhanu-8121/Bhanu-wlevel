#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <Espalexa.h>

// ==== Alexa ====
Espalexa espalexa;

// ==== OTA on port 81 ====
ESP8266WebServer server(81);
ESP8266HTTPUpdateServer httpUpdater;

// ==== WEB SERIAL MONITOR on port 82 ====
ESP8266WebServer logServer(82);
String serialBuffer = "";

// ==== WIFI / TIME ====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Icons
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14;
const int sensor2 = 12;
const int sensor3 = 13;
const int sensor4 = 4;
const int relayPin = 16;
const int switchPin = 2;

// State
bool wifiOK = false;
unsigned long connectStartMillis = 0;
bool apModeLaunched = false;

bool motorON = false;
unsigned long motorTime = 0;

bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

unsigned long blinkTicker = 0;
bool blinkState = false;

String globalLevel = "0%";
int lastSwitchState = HIGH;

// ===== Logs =====
void addLog(const String& msg) {
  Serial.println(msg);
  serialBuffer += msg + "\n";
  if (serialBuffer.length() > 8000) serialBuffer.remove(0, 3000);
}

// ===== Motor control =====
void requestMotorOn(const String& source, const String& level) {
  if (level == "100%") {
    motorON = false;
    digitalWrite(relayPin, LOW);
    addLog("BLOCKED: Tank full → ON rejected (" + source + ")");
    return;
  }
  motorON = true;
  digitalWrite(relayPin, HIGH);
  motorTime = millis();
  addLog("Motor ON by " + source);
}

void requestMotorOff(const String& source) {
  motorON = false;
  digitalWrite(relayPin, LOW);
  addLog("Motor OFF by " + source);
}

// ===== Alexa callback =====
void alexaCallback(uint8_t brightness) {
  String level = globalLevel;
  if (brightness == 0) {
    requestMotorOff("Alexa");
  } else {
    if (level == "100%") {
      motorON = false;
      digitalWrite(relayPin, LOW);
      addLog("Alexa tried ON → BLOCKED (full tank)");
    } else {
      requestMotorOn("Alexa", level);
    }
  }
}

void setupAlexa() {
  espalexa.addDevice("Water Motor", alexaCallback);
  espalexa.begin();
  addLog("Alexa device added: Water Motor");
}

// ===== WiFiManager AP callback =====
void configModeCallback(WiFiManager *wm) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Enter AP Mode");
  lcd.setCursor(0, 1); lcd.print("SSID:");
  lcd.setCursor(5, 1); lcd.print(wm->getConfigPortalSSID());
  addLog("Config Portal Started: " + wm->getConfigPortalSSID());
}

// ===== OTA =====
void setupWebOTA() {
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
  addLog("OTA Ready: http://" + WiFi.localIP().toString() + ":81/update");
}

// ===== Web log =====
void setupWebLogServer() {
  logServer.on("/log", HTTP_GET, []() {
    logServer.send(200, "text/plain", serialBuffer);
  });
  logServer.begin();
  addLog("Web Serial Log ready: http://" + WiFi.localIP().toString() + ":82/log");
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  pinMode(switchPin, INPUT_PULLUP);

  Wire.begin(0, 5); // adjust SDA/SCL pins if needed
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000);
  lcd.clear();

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(1);
  lcd.setCursor(11,1); lcd.print("--:--");

  WiFi.setAutoReconnect(true);
  WiFi.begin();
  connectStartMillis = millis();

  addLog("Booting...");
  setupAlexa();
}

// ===== Loop =====
void loop() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // AP/Config Portal after 30s if not connected (one-time)
  if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {
    addLog("30s timeout → Launching Config Portal");
    WiFiManager wm;
    wm.setAPCallback(configModeCallback);
    wm.setConfigPortalTimeout(180); // 3 minutes
    wm.startConfigPortal("KBC-Setup", "12345678");
    apModeLaunched = true;
    isConnected = (WiFi.status() == WL_CONNECTED);
  }

  if (isConnected) server.handleClient();
  logServer.handleClient();
  espalexa.loop();

  // On connect (once)
  if (isConnected && !wifiOK) {
    wifiOK = true;
    timeClient.begin();
    setupWebOTA();
    setupWebLogServer();
    apModeLaunched = true;

    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(0,1); lcd.print("WiFi Connected   "); // padded
    delay(1500);
  }

  // On disconnect (once)
  if (!isConnected && wifiOK) {
    wifiOK = false;
    lcd.setCursor(0,1); lcd.print("WiFi Disconnect  ");
    delay(1500);
  }

  // NTP sync
  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
  }

  // Build time string
  String currentTime = "--:--";
  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis)/1000;
    unsigned long total = offsetSeconds + elapsed;
    int h = (total/3600) % 24;
    int m = (total/60) % 60;
    char tb[6];
    sprintf(tb, "%02d:%02d", h, m);
    currentTime = String(tb);
  }

  // Read sensors (debounced)
  bool s1=false,s2=false,s3=false; int s4c=0;
  for (int i=0; i<7; i++) {
    if (digitalRead(sensor1)==LOW) s1=true;
    if (digitalRead(sensor2)==LOW) s2=true;
    if (digitalRead(sensor3)==LOW) s3=true;
    if (digitalRead(sensor4)==LOW) s4c++;
    delay(10);
  }
  bool s4 = (s4c>=5);

  // Level mapping
  String level;
  if (s4&&s3&&s2&&s1) level="100%";
  else if (s3&&s2&&s1) level="75%";
  else if (s2&&s1)     level="50%";
  else if (s1)         level="25%";
  else                 level="0%";

  globalLevel = level;

  // LCD top line
  lcd.setCursor(0,0);
  lcd.print("Water Level:     "); // clear remainder
  lcd.setCursor(12,0);
  lcd.print(level);

  // Auto motor logic
  if (level=="0%" && !motorON) requestMotorOn("System", level);
  if (level=="100%" && motorON) requestMotorOff("System");

  // Manual switch
  int sw = digitalRead(switchPin);
  if (sw != lastSwitchState) {
    lastSwitchState = sw;
    if (sw == LOW) {
      if (motorON) requestMotorOff("Switch");
      else if (level != "100%") requestMotorOn("Switch", level);
    }
  }

  // Update LCD motor status
  lcd.setCursor(0,1);
  lcd.print("Motor:");
  lcd.print(motorON ? "ON " : "OFF");
  lcd.print(" ");
  lcd.write(wifiOn[0] ? 0 : 1); // Show WiFi icon
  lcd.setCursor(10,1);
  lcd.print(currentTime);

  delay(100);
}

