#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// CHANGE WIFI
const char* ssid = "KBC Hotspot_EXT";
const char* password = "fpMD@123";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);  // IST

LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Symbols
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// PINS
const int sensor1   = 14;  // D5
const int sensor2   = 12;  // D6
const int sensor3   = 13;  // D7
const int sensor4   = 4;   // D2


const int relayPin  = 16;  // D0

// WiFi State
bool wifiOK = false;
bool firstAttempt = true;
bool showConnectedMsg = false;
bool showFailedMsg = false;
bool showDisconnectedMsg = false;
unsigned long wifiStartTime = 0;
unsigned long msgShowTime = 0;
const unsigned long wifiTimeout = 30000;  // 30 seconds

unsigned long blinkTime = 0;
bool blinkState = false;

bool motorON = false;
unsigned long motorTime = 0;

// === LOCAL RTC (Runs even without WiFi) ===
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;  // Seconds since sync

void setup() {
  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  Wire.begin(0, 5);
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  // Splash
  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000); lcd.clear();

  // Initial Display
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(0);
  lcd.setCursor(11,1); lcd.print("--:--");

  // Start WiFi
  WiFi.begin(ssid, password);
  firstAttempt = true;
  wifiStartTime = millis();
}

void loop() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // === FIRST 2 MIN ATTEMPT ===
  if (firstAttempt && !isConnected && (millis() - wifiStartTime < wifiTimeout)) {
    if (millis() - blinkTime > 500) {
      blinkState = !blinkState;
      blinkTime = millis();
      lcd.setCursor(10,1);
      lcd.write(blinkState ? 0 : ' ');
    }
  }
  // === 2 MIN FAILED ===
  else if (firstAttempt && !isConnected && (millis() - wifiStartTime >= wifiTimeout)) {
    firstAttempt = false;
    showFailedMsg = true;
    msgShowTime = millis();
    lcd.setCursor(0,1); lcd.print("WiFi Failed     ");
    delay(1000);
    lcd.setCursor(0,1); lcd.print("Motor:OFF       ");
    lcd.setCursor(10,1); lcd.write(1);
  }

  // === WIFI CONNECTED ===
  if (isConnected && !wifiOK) {
    wifiOK = true;
    firstAttempt = false;
    showConnectedMsg = true;
    msgShowTime = millis();
    timeClient.begin();
    timeClient.update();
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours() * 3600 + timeClient.getMinutes() * 60 + timeClient.getSeconds();
    lcd.setCursor(0,1); lcd.print("WiFi Connected  ");
    delay(1000);
    lcd.setCursor(0,1); lcd.print("                ");
  }

  // === WIFI DISCONNECTED AFTER CONNECT ===
  if (!isConnected && wifiOK) {
    wifiOK = false;
    showDisconnectedMsg = true;
    msgShowTime = millis();
    lcd.setCursor(0,1); lcd.print("WiFi Disconnected");
    delay(1000);
    lcd.setCursor(0,1); lcd.print("Motor:OFF       ");
    lcd.setCursor(10,1); lcd.write(1);
  }

  // === UPDATE LOCAL TIME (Even without WiFi) ===
  String currentTime = "--:--";
  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis) / 1000;
    unsigned long totalSeconds = offsetSeconds + elapsed;
    int h = (totalSeconds / 3600) % 24;
    int m = (totalSeconds / 60) % 60;
    char buf[6];
    sprintf(buf, "%02d:%02d", h, m);
    currentTime = String(buf);
  }

  // === SENSOR READ ===
  bool s1 = false, s2 = false, s3 = false, s4 = false;
  int s4c = 0;
  for (int i = 0; i < 7; i++) {
    if (digitalRead(sensor1) == LOW) s1 = true;
    if (digitalRead(sensor2) == LOW) s2 = true;
    if (digitalRead(sensor3) == LOW) s3 = true;
    if (digitalRead(sensor4) == LOW) s4c++;
    delay(10);
  }
  s4 = (s4c >= 5);

  String level;
  if (s4 && s3 && s2 && s1) level = "100%";
  else if (s3 && s2 && s1) level = "75%";
  else if (s2 && s1) level = "50%";
  else if (s1) level = "25%";
  else level = "0%";

  lcd.setCursor(12,0); lcd.print("    "); lcd.setCursor(12,0); lcd.print(level);

  // === RELAY ===
  if (level == "0%" && !motorON) { motorON = true; digitalWrite(relayPin, HIGH); motorTime = millis(); }
  if (level == "100%" && motorON) { motorON = false; digitalWrite(relayPin, LOW); }

  // === DISPLAY ===
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[16]; sprintf(buf, "Motor:ON frm %02dM", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);

    if (wifiOK) {
      lcd.write(0);
    } else if (firstAttempt) {
      if (millis() - blinkTime > 500) {
        blinkState = !blinkState;
        blinkTime = millis();
      }
      lcd.write(blinkState ? 0 : ' ');
    } else {
      lcd.write(1);
    }

    lcd.setCursor(11,1); lcd.print(currentTime);  // Always updated time
  }

  delay(200);
}
