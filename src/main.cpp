#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>  // WiFiManager Mode 3 Try WiFi first → only open AP if connection really fails.
#include <NTPClient.h>
#include <WiFiUdp.h>

// ===== WEB OTA =====
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>

// ===== ALEXA WEMO =====
#include <fauxmoESP.h>

// ===== OBJECTS =====
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
fauxmoESP fauxmo;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST

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

// ===== WiFi Manager function (Mode 3) =====
void startWiFiManager() {

  WiFiManager wm;

  wm.setTimeout(15);                  // Try saved WiFi for 15 sec
  bool ok = wm.autoConnect("KBC-Setup");

  if (!ok) {
    Serial.println("❌ WiFi failed → Starting AP Mode");
  } else {
    Serial.println("✅ WiFi Connected");
  }
}

// ===== Web OTA =====
void setupWebOTA() {
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
  Serial.println("WebOTA Ready → http://DEVICE-IP/update");
}

// ===== Alexa Callback =====
void wemoCallback(unsigned char device_id,
                  const char * device_name,
                  bool state,
                  unsigned char value) {

  Serial.printf("Alexa → %s = %s\n",
                device_name,
                state ? "ON" : "OFF");

  motorON = state;
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nBooting KBC Controller…");

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(manualPin, INPUT_PULLUP);

  digitalWrite(relayPin, LOW);

  // LCD
  Wire.begin(0, 5);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0); lcd.print("KBC Controller");
  lcd.setCursor(0,1); lcd.print("Starting…");
  delay(1500);
  lcd.clear();

  // WiFiManager (Mode 3)
  startWiFiManager();

  // Start NTP
  timeClient.begin();

  // Web OTA
  setupWebOTA();

  // Alexa Wemo
  fauxmo.createServer(true);
  fauxmo.setPort(80);
  fauxmo.enable(true);
  fauxmo.addDevice("Motor");
  fauxmo.onSetState(wemoCallback);
}

// ===== LOOP =====
void loop() {
  server.handleClient();
  fauxmo.handle();

  bool isConnected = WiFi.status() == WL_CONNECTED;

  if (isConnected && !wifiOK) {
    wifiOK = true;
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }

  if (isConnected) {
    timeClient.update();
  }

  // Manual Switch
  if (digitalRead(manualPin) == LOW) {
    delay(120);
    if (digitalRead(manualPin) == LOW) {
      motorON = !motorON;
      Serial.println("Manual Toggle");
      delay(300);
    }
  }

  // Water Sensors
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

  // Safety
  if (level == "0%") motorON = true;
  if (level == "100%") motorON = false;

  // Motor relay
  if (motorON && !lastMotorState) {
    motorTime = millis();
    Serial.println("Motor ON");
  }
  if (!motorON && lastMotorState) {
    Serial.println("Motor OFF");
  }
  lastMotorState = motorON;
  digitalWrite(relayPin, motorON ? HIGH : LOW);

  // LCD update
  lcd.setCursor(0,0);
  lcd.print("Level: ");
  lcd.print(level);
  lcd.print("   ");

  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    lcd.print("Motor ON ");
    lcd.print(mins);
    lcd.print("m   ");
  } else {
    lcd.print("Motor OFF ");
    lcd.print(timeClient.getFormattedTime());
  }

  delay(10);
}
