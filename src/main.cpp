/* Water Motor Control with Alexa (Legacy Espalexa)
   - Alexa on port 80
   - OTA on port 81
   - Web log on port 82
   - LCD + Sensors + Manual Switch
   - Auto ON at 0%, Auto OFF at 100%
   - Tank full lock for Alexa
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

// ===== Alexa (port 80) =====
Espalexa espalexa;
ESP8266WebServer alexaServer(80);

// ===== OTA (port 81) =====
ESP8266WebServer server(81);
ESP8266HTTPUpdateServer httpUpdater;

// ===== Web Log (port 82) =====
ESP8266WebServer logServer(82);
String serialBuffer = "";

// Append log to buffer + Serial
void addLog(String s) {
  Serial.println(s);
  serialBuffer += s + "\n";
  if (serialBuffer.length() > 8000) serialBuffer.remove(0, 3000);
}

// ===== Time Sync =====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);
