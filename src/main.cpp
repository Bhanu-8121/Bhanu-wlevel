#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>

#include <fauxmoESP.h>

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
fauxmoESP fauxmo;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST timezone

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ===== GPIO PINS =====
const int sensor1 = 14;  // D5
const int sensor2 = 12;  // D6
const int sensor3 = 13;  // D7
const int sensor4 = 4;   // D2
const int relayPin = 16; // D0
const int manualPin = 2; // D4

// ===== STATE =====
bool wifiOK = false;
bool motorON = false;
bool lastMotorState = false;

unsigned long motorTime = 0;

// ===== Symbols =====
byte wifiSym[8] = {0x04,0x0E,0x15,0x04,0x04,0x00,0x00,0x00};     // Connected
byte wifiBlinkSym[8] = {0x00,0x04,0x0E,0x04,0x00,0x00,0x00,0x00}; // Searching
byte wifiFailSym[8] = {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00,0x00};  // Failed

// ==========================
// WiFi Manager MODE 3 LOGIC
// ==========================
bool startWiFiManager() {

    WiFiManager wm;
    wm.setTimeout(15);     // Try saved WiFi for 15 sec

    unsigned long start = millis();
    bool ok = false;

    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi Connect…");

    // ===== TRY SAVED WIFI FIRST (15 sec) =====
    while (millis() - start < 15000) {
        lcd.setCursor(15,1);
        lcd.write((millis() / 500) % 2 ? 1 : 0);  // blink
        if (WiFi.status() == WL_CONNECTED) {
            ok = true;
            break;
        }
        delay(200);
    }

    if (ok) return true;

    // =====================
    // ENTER AP MODE (2 min)
    // =====================
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("AP: KBC-Setup");

    unsigned long apStart = millis();
    bool messageShown = false;

    wm.startConfigPortal("KBC-Setup");

    while (millis() - apStart < 120000) {

        // Blink WiFi symbol
        lcd.setCursor(15,1);
        lcd.write((millis() / 500) % 2 ? 1 : 0);

        // Scroll message after 30 seconds
        if (!messageShown && millis() - apStart > 30000) {
            messageShown = true;

            for (int p = 0; p < 16; p++) {
                lcd.setCursor(0,1);
                lcd.print("Connect to KBC   " + String(p));
                delay(200);
            }
            lcd.setCursor(0,1);
            lcd.print("AP Ready       ");
        }

        if (WiFi.status() == WL_CONNECTED) {
            return true;
        }
        delay(150);
    }

    // timeout 2 min → exit AP and run normally
    return WiFi.status() == WL_CONNECTED;
}

// ======================
// WEB OTA
// ======================
void setupWebOTA() {
    httpUpdater.setup(&server, "/update", "kbc", "987654321");
    server.begin();
}

// ======================
// ALEXA CALLBACK
// ======================
void wemoCallback(unsigned char device_id,
                  const char* device_name,
                  bool state,
                  unsigned char value) {
    motorON = state;
}

// ======================
// SETUP
// ======================
void setup() {
    Serial.begin(115200);

    pinMode(sensor1, INPUT_PULLUP);
    pinMode(sensor2, INPUT_PULLUP);
    pinMode(sensor3, INPUT_PULLUP);
    pinMode(sensor4, INPUT_PULLUP);
    pinMode(relayPin, OUTPUT);
    pinMode(manualPin, INPUT_PULLUP);

    digitalWrite(relayPin, LOW);

    // LCD
    Wire.begin(0,5);
    lcd.init();
    lcd.backlight();

    lcd.createChar(0, wifiSym);
    lcd.createChar(1, wifiBlinkSym);
    lcd.createChar(2, wifiFailSym);

    lcd.setCursor(0,0); lcd.print("KBC Controller");
    lcd.setCursor(0,1); lcd.print("Starting…");
    delay(1200);

    // WiFi
    wifiOK = startWiFiManager();

    // NTP
    timeClient.begin();

    // OTA
    setupWebOTA();

    // Alexa
    fauxmo.createServer(true);
    fauxmo.setPort(80);
    fauxmo.enable(true);
    fauxmo.addDevice("Motor");
    fauxmo.onSetState(wemoCallback);
}

// ======================
// LOOP
// ======================
void loop() {

    server.handleClient();
    fauxmo.handle();

    bool isConnected = WiFi.status() == WL_CONNECTED;

    if (isConnected) timeClient.update();

    // Manual Toggle
    if (digitalRead(manualPin) == LOW) {
        delay(120);
        if (digitalRead(manualPin) == LOW) {
            motorON = !motorON;
            delay(300);
        }
    }

    // Water sensors
    bool s1 = digitalRead(sensor1) == LOW;
    bool s2 = digitalRead(sensor2) == LOW;
    bool s3 = digitalRead(sensor3) == LOW;
    bool s4 = digitalRead(sensor4) == LOW;

    String level;
    if (s4 && s3 && s2 && s1) level = "100%";
    else if (s3 && s2 && s1) level = "75%";
    else if (s2 && s1) level = "50%";
    else if (s1) level = "25%";
    else level = "0%";

    if (level == "0%") motorON = true;
    if (level == "100%") motorON = false;

    // Motor relay
    if (motorON && !lastMotorState) motorTime = millis();
    lastMotorState = motorON;
    digitalWrite(relayPin, motorON ? HIGH : LOW);

    // ============================
    // LCD UPDATE
    // ============================
    lcd.setCursor(0,0);
    lcd.print("Level:");
    lcd.print(level);
    lcd.print("   ");

    lcd.setCursor(0,1);

    if (!isConnected) {
        lcd.print("WFail ");
        lcd.write(2);  // error symbol
    } else {
        if (motorON) {
            int mins = (millis() - motorTime) / 60000;
            lcd.print("MOn ");
            if (mins < 10) lcd.print("0");
            lcd.print(mins);
        } else {
            lcd.print(timeClient.getFormattedTime().substring(0,5));
            lcd.print("  ");
        }
    }

    delay(50);
}
