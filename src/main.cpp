/*********************************************************************
 *  FINAL CODE – WiFi Manager + AUTO CAPTIVE PORTAL (POP-UP)
 *  → Connect to "KBC-Setup" → Portal khud khulega!
 *********************************************************************/


#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          // ← REAL WiFiManager
#include <ESP8266HTTPUpdateServer.h>
#include <Espalexa.h>

// ================== PINS ==================
const int sensor1 = 14;  // D5
const int sensor2 = 12;  // D6
const int sensor3 = 13;  // D7
const int sensor4 = 4;   // D2
const int relayPin = 16; // D0 → Active HIGH
const int switchPin = 15; // D8 → SAFE

// ================== OBJECTS ==================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);
LiquidCrystal_I2C lcd(0x27, 16, 2);
ESP8266WebServer server(81);
ESP8266HTTPUpdateServer httpUpdater;
Espalexa espalexa;

// WiFi Icons
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// ================== VARIABLES ==================
bool wifiOK = false;
bool motorON = false;
unsigned long motorTime = 0;
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;
String globalLevel = "0%";
int lastSwitchState = HIGH;

// ================== WiFiManager CALLBACK ==================
void configModeCallback(WiFiManager *myWiFiManager) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Connect WiFi:");
  lcd.setCursor(0, 1); lcd.print("KBC-Setup");
  Serial.println("=== CAPTIVE PORTAL ACTIVE ===");
  Serial.println("SSID: KBC-Setup");
  Serial.println("Open browser → portal will auto popup!");
}

void saveConfigCallback() {
  Serial.println("WiFi Config Saved! Restarting...");
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("WiFi Saved!");
  lcd.setCursor(0,1); lcd.print("Restarting...");
  delay(2000);
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(switchPin, INPUT_PULLUP);
  digitalWrite(relayPin, LOW);

  // LCD Init
  Wire.begin(0, 5);
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000);
  lcd.clear();

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Starting...");
  
  // === REAL WiFiManager ===
  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);  // 3 minutes
  wm.setCaptivePortalEnable(true); // ← YEHI LINE AUTO POP-UP DETECTOR ON KARTA HAI

  // Agar pehle se saved WiFi hai → connect karega
  // Nahi toh → "KBC-Setup" AP banega + CAPTIVE PORTAL auto khulega
  if (!wm.autoConnect("KBC-Setup", "12345678")) {
    Serial.println("Failed to connect & timeout");
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("WiFi Failed");
    lcd.setCursor(0,1); lcd.print("Restarting...");
    delay(3000);
    ESP.restart();
  }

  // Agar yahan tak aaya → WiFi connected hai!
  wifiOK = true;
  Serial.println("WiFi Connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("WiFi Connected ");
  delay(1500);

  // Start services
  timeClient.begin();
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
  espalexa.addDevice("Water Motor", [](uint8_t brightness) {
    if (brightness == 255 && globalLevel != "100%") {
      digitalWrite(relayPin, HIGH);
      motorON = true; motorTime = millis();
    } else {
      digitalWrite(relayPin, LOW);
      motorON = false;
    }
  });
  espalexa.begin();
}

// ================== LOOP ==================
void loop() {
  server.handleClient();
  espalexa.loop();

  bool isConnected = (WiFi.status() == WL_CONNECTED);
  if (!isConnected && wifiOK) {
    wifiOK = false;
    lcd.setCursor(0,1); lcd.print("WiFi Lost!     ");
    delay(2000);
  }
  if (isConnected && !wifiOK) {
    wifiOK = true;
    lcd.setCursor(0,1); lcd.print("WiFi Reconnected");
    delay(1500);
  }

  // NTP
  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
  }

  String currentTime = "--:--";
  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis)/1000;
    unsigned long total = offsetSeconds + elapsed;
    int h = (total/3600)%24, m = (total/60)%60;
    char t[6]; sprintf(t, "%02d:%02d", h, m);
    currentTime = t;
  }

  // Sensors
  bool s1 = false, s2 = false, s3 = false; int s4c = 0;
  for(int i=0; i<7; i++) {
    if(digitalRead(sensor1)==LOW) s1=true;
    if(digitalRead(sensor2)==LOW) s2=true;
    if(digitalRead(sensor3)==LOW) s3=true;
    if(digitalRead(sensor4)==LOW) s4c++;
    delay(10);
  }
  bool s4 = s4c >= 5;
  String level = s4&&s3&&s2&&s1 ? "100%" :
                 s3&&s2&&s1 ? "75%" :
                 s2&&s1 ? "50%" :
                 s1 ? "25%" : "0%";
  globalLevel = level;

  lcd.setCursor(12,0); lcd.print("    "); lcd.setCursor(12,0); lcd.print(level);

  // Auto Control
  if (level == "0%" && !motorON) { digitalWrite(relayPin, HIGH); motorON = true; motorTime = millis(); }
  if (level == "100%" && motorON) { digitalWrite(relayPin, LOW); motorON = false; }

  // Manual Switch
  int sw = digitalRead(switchPin);
  if (lastSwitchState == HIGH && sw == LOW) {
    digitalWrite(relayPin, !motorON);
    motorON = !motorON;
    if (motorON) motorTime = millis();
    delay(200);
  }
  lastSwitchState = sw;

  // Display
  lcd.setCursor(0,1);
  lcd.print("                ");
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[17];
    snprintf(buf, sizeof(buf), "Motor:ON frm %02dM", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1); lcd.write(wifiOK ? 0 : 1);
    lcd.setCursor(11,1); lcd.print(currentTime);
  }

  delay(200);
}
