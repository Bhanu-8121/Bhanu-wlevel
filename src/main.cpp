/*
   Water Motor Automation with Alexa (Legacy Espalexa)
   ---------------------------------------------------
   - Alexa control (brightness callback)
   - OTA on port 81
   - Logs on port 82
   - WiFiManager AP only if no WiFi in 30 sec
   - LCD + Sensors + Auto Logic + Manual Switch
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

// === Alexa Server ===
ESP8266WebServer alexaServer(80);
Espalexa espalexa;

// === OTA on port 81 ===
ESP8266WebServer server(81);
ESP8266HTTPUpdateServer httpUpdater;

// === Log Server on port 82 ===
ESP8266WebServer logServer(82);
String serialBuffer = "";

// Log handler
void addLog(String s) {
  Serial.println(s);
  serialBuffer += s + "\n";
  if (serialBuffer.length() > 8000) serialBuffer.remove(0, 3000);
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
const int sensor1 = 14;
const int sensor2 = 12;
const int sensor3 = 13;
const int sensor4 = 4;
const int relayPin = 16;
const int switchPin = 2;

// === States ===
bool wifiOK = false;
bool apModeLaunched = false;
unsigned long connectStartMillis = 0;

bool motorON = false;
unsigned long motorTime = 0;
String globalLevel = "0%";
int lastSwitchState = HIGH;

// -----------------------------------------------------
// MOTOR CONTROL
// -----------------------------------------------------
void requestMotorOn(String source, String level)
{
  if (level == "100%") {
    motorON = false;
    digitalWrite(relayPin, LOW);
    addLog("BLOCK: Alexa/System tried ON but tank is FULL");
    return;
  }

  motorON = true;
  digitalWrite(relayPin, HIGH);
  motorTime = millis();
  addLog("Motor ON by " + source);
}

void requestMotorOff(String source)
{
  motorON = false;
  digitalWrite(relayPin, LOW);
  addLog("Motor OFF by " + source);
}

// -----------------------------------------------------
// ALEXA CALLBACK (brightness 0–255)
// -----------------------------------------------------
void alexaCallback(uint8_t brightness)
{
  String level = globalLevel;

  if (brightness == 0) {
    requestMotorOff("Alexa");
  }
  else {
    if (level == "100%") {
      addLog("Alexa ON → BLOCKED (100% full)");
      digitalWrite(relayPin, LOW);
    } else {
      requestMotorOn("Alexa", level);
    }
  }
}

// -----------------------------------------------------
// ALEXA SETUP
// -----------------------------------------------------
void setupAlexa()
{
  espalexa.addDevice("Water Motor", alexaCallback);
  espalexa.begin(&alexaServer);
  addLog("Alexa device registered");
}

// -----------------------------------------------------
// WiFiManager AP Callback
// -----------------------------------------------------
void configModeCallback(WiFiManager *wm)
{
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("AP MODE");
  lcd.setCursor(0,1); lcd.print(wm->getConfigPortalSSID());
  addLog("WiFiManager AP: " + wm->getConfigPortalSSID());
}

// -----------------------------------------------------
// OTA + LOG SERVER
// -----------------------------------------------------
void setupWebOTA()
{
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
  addLog("OTA Ready: http://" + WiFi.localIP().toString() + ":81/update");
}

void setupWebLogServer()
{
  logServer.on("/log", HTTP_GET, []() {
    logServer.send(200, "text/plain", serialBuffer);
  });
  logServer.begin();
  addLog("Log Server Ready: http://" + WiFi.localIP().toString() + ":82/log");
}

// -----------------------------------------------------
// SETUP
// -----------------------------------------------------
void setup()
{
  Serial.begin(115200);
  addLog("Booting...");

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  pinMode(switchPin, INPUT_PULLUP);

  Wire.begin(0,5);
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

  setupAlexa();
}

// -----------------------------------------------------
// LOOP
// -----------------------------------------------------
void loop()
{
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // --- WiFiManager AP Mode ---
  if (!isConnected && !apModeLaunched && millis() - connectStartMillis > 30000)
  {
    WiFiManager wm;
    wm.setAPCallback(configModeCallback);
    wm.setConfigPortalTimeout(180);
    wm.startConfigPortal("KBC-Setup", "12345678");
    apModeLaunched = true;
    isConnected = (WiFi.status() == WL_CONNECTED);
  }

  // --- When WiFi connects ---
  if (isConnected && !wifiOK)
  {
    wifiOK = true;
    timeClient.begin();
    setupWebOTA();
    setupWebLogServer();

    addLog("WiFi Connected: " + WiFi.localIP().toString());

    lcd.clear();
    lcd.setCursor(0,1); lcd.print("WiFi Connected");
    delay(1500);
  }

  if (isConnected) server.handleClient();
  logServer.handleClient();
  alexaServer.handleClient();
  espalexa.loop();

  // --- Time Sync ---
  if (isConnected && timeClient.update())
  {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds =
      timeClient.getSeconds() +
      timeClient.getMinutes()*60 +
      timeClient.getHours()*3600;
  }

  // --- Compute Time ---
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

  // --- Read Sensors ---
  bool s1=false,s2=false,s3=false;
  int s4c=0;
  for (int i=0;i<7;i++){
    if (!digitalRead(sensor1)) s1=true;
    if (!digitalRead(sensor2)) s2=true;
    if (!digitalRead(sensor3)) s3=true;
    if (!digitalRead(sensor4)) s4c++;
    delay(10);
  }
  bool s4 = (s4c >= 5);

  String level;
  if (s4&&s3&&s2&&s1) level="100%";
  else if (s3&&s2&&s1) level="75%";
  else if (s2&&s1) level="50%";
  else if (s1) level="25%";
  else level="0%";

  globalLevel = level;

  lcd.setCursor(12,0);
  lcd.print(level + " ");

  // --- Auto Logic ---
  if (level=="0%" && !motorON) requestMotorOn("System", level);
  if (level=="100%" && motorON) requestMotorOff("System");

  // --- Manual switch ---
  int sw = digitalRead(switchPin);
  if (lastSwitchState == HIGH && sw == LOW)
  {
    if (motorON) requestMotorOff("Switch");
    else requestMotorOn("Switch", level);
    delay(80);
  }
  lastSwitchState = sw;

  // --- LCD Bottom Row ---
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis()-motorTime)/60000;
    char buf[16];
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
