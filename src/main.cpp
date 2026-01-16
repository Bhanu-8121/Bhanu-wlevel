/* Water motor Alexa - FINAL CORRECTED VERSION
 * - ALLOWS ON command at 100% (Manual Force)
 * - 30 Minute Safety Auto-Shutoff for ALL modes
 * - Web Serial Monitor on Port 82
 * - OTA Update on Port 81
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

String lastLoggedLevel = "";
// 30 Minutes = 30 * 60 * 1000 milliseconds
const unsigned long MAX_MOTOR_RUNTIME = 1800000; 

// ==== Alexa ====
Espalexa espalexa;
bool alexaStarted = false; 

// ==== OTA on port 81 ====
ESP8266WebServer server(81);
ESP8266HTTPUpdateServer httpUpdater;

// ==== WEB SERIAL MONITOR on port 82 ====
ESP8266WebServer logServer(82);
String serialBuffer = "";

bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800);

void addLog(String msg) {
  time_t rawtime = timeClient.getEpochTime();
  struct tm * ti = localtime(&rawtime);

  char dateBuffer[25];
  sprintf(dateBuffer, "%02d-%02d-%04d %02d:%02d:%02d", 
          ti->tm_mday, ti->tm_mon + 1, ti->tm_year + 1900, 
          ti->tm_hour, ti->tm_min, ti->tm_sec);

  String line = "[" + String(dateBuffer) + "] " + msg;
  serialBuffer += line + "\n";
  
  if (serialBuffer.length() > 5000) {
    serialBuffer = serialBuffer.substring(serialBuffer.indexOf('\n', 500) + 1);
  }
}

LiquidCrystal_I2C lcd(0x27, 16, 2);

byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14; // D5
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 5;  // D1
const int relayPin = 16; // D0
const int switchPin = 3; // RX

bool wifiOK = false;
unsigned long connectStartMillis = 0;
bool apModeLaunched = false; 

// Motor state
bool motorON = false;
unsigned long motorStartTime = 0;
bool manualForce = false; // Prevents auto-shutoff at 100% if manually started

String globalLevel = "0%";
int lastSwitchState = HIGH;

unsigned long blinkTicker = 0;
bool blinkState = false;

// =========================================
//        MOTOR CONTROL LOGIC
// =========================================
void requestMotorOn(String source, String level)
{
  motorON = true;
  digitalWrite(relayPin, HIGH);
  motorStartTime = millis(); // Start the 30-min timer
  
  // If human turns it on, set manualForce to true so it doesn't stop at 100%
  if (source == "Alexa" || source == "Switch") {
      manualForce = true;
  } else {
      manualForce = false;
  }

  addLog("Motor ON by " + source + " | Level: " + level); 
}

void requestMotorOff(String source)
{
  motorON = false;
  manualForce = false;
  digitalWrite(relayPin, LOW);
  addLog("Motor OFF by " + source + " | Level: " + globalLevel);
}

// =========================================
//              ALEXA CALLBACK
// =========================================
void alexaCallback(uint8_t brightness)
{
  // COMPLETELY REMOVED BLOCKING LOGIC
  if (brightness == 0) {
    requestMotorOff("Alexa");
  } else {
    requestMotorOn("Alexa", globalLevel);
  }
}

void setupAlexa()
{
  espalexa.addDevice("Water Motor", alexaCallback);
  addLog("Alexa device registered.");
}

void setupWebLogServer()
{
  logServer.on("/log", HTTP_GET, []() {
    String motorStatus = motorON ? "ON " + String((millis() - motorStartTime) / 60000) + "M" : "OFF";
    String webOutput = "--- LIVE STATUS ---\n";
    webOutput += "Level: " + globalLevel + "\n";
    webOutput += "Motor: " + motorStatus + "\n";
    if(manualForce) webOutput += "Mode: MANUAL OVERRIDE (30m max)\n";
    webOutput += "-------------------\n\n";
    webOutput += "--- SYSTEM LOGS ---\n" + serialBuffer;
    logServer.send(200, "text/plain", webOutput);
  });
  logServer.begin();
}

void setup()
{
  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(switchPin, INPUT_PULLUP);
  digitalWrite(relayPin, LOW);

  Wire.begin(0,2);
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn); lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(1500);
  lcd.clear();

  WiFi.setAutoReconnect(true);
  WiFi.begin(); 
  connectStartMillis = millis();

  setupAlexa();
  timeClient.begin();
  
  unsigned long startSync = millis();
  while (!timeClient.update() && millis() - startSync < 5000) { delay(200); }
  
  addLog("System Booted. Level: " + globalLevel);
}

void loop()
{
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (isConnected) server.handleClient();
  logServer.handleClient();
  if (alexaStarted) espalexa.loop();

  if (isConnected && !wifiOK) {
    wifiOK = true;
    httpUpdater.setup(&server, "/update", "kbc", "987654321");
    server.begin();
    setupWebLogServer();
    if (!alexaStarted) { espalexa.begin(); alexaStarted = true; }
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("WiFi Online");
    delay(1000);
    lcd.clear();
  }

  // --- NTP Time ---
  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
  }

  // --- Sensors ---
  bool s1 = (digitalRead(sensor1) == LOW);
  bool s2 = (digitalRead(sensor2) == LOW);
  bool s3 = (digitalRead(sensor3) == LOW);
  bool s4 = (digitalRead(sensor4) == LOW);

  String level;
  if (s4)      level = "100%";
  else if (s3) level = "75%";
  else if (s2) level = "50%";
  else if (s1) level = "25%";
  else         level = "0%";

  if (level != lastLoggedLevel) {
    addLog("Water Level: " + level);
    lastLoggedLevel = level; 
  }
  globalLevel = level;
  
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(12,0); lcd.print(level + "  ");

  // --- AUTO LOGIC ---
  // Start if empty
  if (level == "0%" && !motorON) {
    requestMotorOn("System", level);
  }
  
  // Stop at 100% ONLY if it wasn't a manual force command
  if (level == "100%" && motorON && !manualForce) {
    requestMotorOff("System");
  }

  // --- SAFETY TIMER (30 Minutes) ---
  // This triggers no matter how the motor was started
  if (motorON && (millis() - motorStartTime >= MAX_MOTOR_RUNTIME)) {
    requestMotorOff("Safety-Timeout");
  }

  // --- Manual switch ---
  int sw = digitalRead(switchPin);
  if (lastSwitchState == HIGH && sw == LOW) {
    if (motorON) requestMotorOff("Switch");
    else requestMotorOn("Switch", level); // This sets manualForce = true
    delay(150); 
  }
  lastSwitchState = sw;

  // LCD Updates
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorStartTime) / 60000;
    char buf[17];
    sprintf(buf, "Motor:ON %02dM   ", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);
    lcd.write(wifiOK ? 0 : 1);
  }
  
  delay(200);
}

