/*********************************************************************
   K.B.C WATER TANK AUTOMATION – FINAL GOD MODE VERSION
   → WiFi Manager + Auto Captive Portal
   → IP Address Display on LCD when WiFi connects
   → Alexa + Manual Switch + Auto + Live Time + Web OTA + Web Logs
*********************************************************************/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <Espalexa.h>

// ================== PINS ==================
const uint8_t SENSOR1    = 14;  // D5
const uint8_t SENSOR2    = 12;  // D6
const uint8_t SENSOR3    = 13;  // D7
const uint8_t SENSOR4    = 4;   // D2
const uint8_t RELAY_PIN  = 16;  // D0 → Active HIGH
const uint8_t SWITCH_PIN = 15;  // D8 → 100% SAFE

// ================== OBJECTS ==================
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

ESP8266WebServer httpServer(81);
ESP8266HTTPUpdateServer httpUpdater;

ESP8266WebServer logServer(82);
String serialBuffer = "";

Espalexa espalexa;

// ================== WiFi ICONS ==================
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// ================== VARIABLES ==================
bool wifiOK = false;
bool motorON = false;
unsigned long motorTime = 0;
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;
String globalLevel = "0%";
int lastSwitchState = HIGH;

// ================== LOG FUNCTION ==================
void addLog(String msg) {
  String logLine = String(millis()/1000) + "s: " + msg;
  Serial.println(logLine);
  serialBuffer += logLine + "<br>";
  if (serialBuffer.length() > 15000) serialBuffer.remove(0, 5000);
}

// ================== DISPLAY IP WHEN CONNECTED =================
