#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ===== WEB OTA =====
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>

// ===== ALEXA WEMO =====
#include <fauxmoESP.h>

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
fauxmoESP fauxmo;

// ===== WIFI =====
const char* ssid = "KBC Hotspot";
const char* password = "fpMD@143";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);  // IST

LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Symbols
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins (unchanged)
const int sensor1 = 14; // D5
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 4;  // D2
const int relayPin = 16; // D0
const int manualPin = 2; // D4

bool wifiOK = false;
bool firstAttempt = true;
unsigned long wifiStartTime = 0;
const unsigned long wifiTimeout = 30000;

unsigned long blinkTime = 0;
bool blinkState = false;

bool motorON = false;
bool lastMotorState = false;
unsigned long motorTime = 0;

bool lastSwitchState = HIGH;
unsigned long lastSwitchTime = 0;
const unsigned long debounceDelay = 50;

bool timeSynced = false;
unsigned long offsetSeconds = 0;
unsigned long lastSyncMillis = 0;

// ===== WEB OTA SETUP =====
void setupWebOTA() {
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
  Serial.println("WEB OTA Ready: http://<ESP-IP>/update");
}

// ===== WEMO CALLBACK =====
void wemoCallback(unsigned char device_id, const char * device_name, bool state) {
  Serial.printf("Alexa(WEMO) → %s = %s\n", device_name, state ? "ON" : "OFF");
  motorON = state;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nBooting Water Level Controller + Wemo...");

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(manualPin, INPUT_PULLUP);

  digitalWrite(relayPin, LOW);
  lastSwitchState = digitalRead(manualPin);

  Wire.begin(0, 5);
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000); lcd.clear();

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF "); 
  lcd.setCursor(11,1); lcd.print("--:--");

  WiFi.begin(ssid, password);
  wifiStartTime = millis();
  setupWebOTA();

  // --------- ALEXA WEMO DEVICE ----------
  fauxmo.addDevice("Motor");     // ⭐ Alexa Name
  fauxmo.onSetState(wemoCallback);
  fauxmo.setPort(80);
  fauxmo.enable(true);
}

void loop() {
  server.handleClient();
  fauxmo.handle();

  bool isConnected = (WiFi.status() == WL_CONNECTED);
  bool ntpSuccess = false;

  if (isConnected) {
    if (!wifiOK) {
      wifiOK = true;
      firstAttempt = false;

      timeClient.begin();

      lcd.setCursor(0,1); lcd.print("WiFi Connected ");
      delay(1000);
      lcd.setCursor(0,1); lcd.print("               ");

      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    }

    ntpSuccess = timeClient.update();
  }

  if (!isConnected) {
    if (wifiOK) {
      wifiOK = false;
      Serial.println("WiFi Lost. Soft RTC active.");

      lcd.setCursor(0,1); lcd.print("WiFi Disconnect");
      delay(1000);
    }

    if (firstAttempt && (millis() - wifiStartTime >= wifiTimeout)) {
      firstAttempt = false;
      lcd.setCursor(0,1); lcd.print("WiFi Failed    ");
      delay(1000);
    }
  }

  // --------- SOFT RTC ----------
  String currentTime = "--:--";

  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis) / 1000;
    unsigned long totalSeconds = offsetSeconds + elapsed;

    if (ntpSuccess) {
      totalSeconds = timeClient.getHours()*3600 +
                     timeClient.getMinutes()*60 +
                     timeClient.getSeconds();

      offsetSeconds = totalSeconds;
      lastSyncMillis = millis();
    }

    int h = (totalSeconds / 3600) % 24;
    int m = (totalSeconds / 60) % 60;

    char buf[6];
    sprintf(buf, "%02d:%02d", h, m);
    currentTime = String(buf);
  }
  else if (ntpSuccess) {
    timeSynced = true;
    offsetSeconds = timeClient.getHours()*3600 +
                    timeClient.getMinutes()*60 +
                    timeClient.getSeconds();
    lastSyncMillis = millis();
  }

  // --------- MANUAL SWITCH ----------
  bool currentSwitch = digitalRead(manualPin);
  if (currentSwitch != lastSwitchState) lastSwitchTime = millis();

  if ((millis() - lastSwitchTime) > debounceDelay) {
    if (currentSwitch == LOW && lastSwitchState == HIGH) {
      motorON = !motorON;
    }
  }
  lastSwitchState = currentSwitch;

  // --------- WATER LEVEL ----------
  bool s1=false,s2=false,s3=false,s4=false;
  int s4c=0;

  for(int i=0;i<7;i++){
    if (digitalRead(sensor1)==LOW) s1=true;
    if (digitalRead(sensor2)==LOW) s2=true;
    if (digitalRead(sensor3)==LOW) s3=true;
    if (digitalRead(sensor4)==LOW) s4c++;
    delay(10);
  }
  s4 = (s4c>=5);

  String level;
  if (s4&&s3&&s2&&s1) level="100%";
  else if (s3&&s2&&s1) level="75%";
  else if (s2&&s1) level="50%";
  else if (s1) level="25%";
  else level="0%";

  lcd.setCursor(12,0); lcd.print("    ");
  lcd.setCursor(12,0); lcd.print(level);

  // --------- AUTO STOP/START ----------
  if (level == "0%") motorON = true;
  if (level == "100%") motorON = false;

  // --------- RELAY ----------
  if (motorON && !lastMotorState) motorTime = millis();
  lastMotorState = motorON;

  digitalWrite(relayPin, motorON ? HIGH : LOW);

  // --------- LCD ----------
  lcd.setCursor(0,1);

  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[17];
    sprintf(buf, "Motor:ON frm %02dM", mins);
    lcd.print(buf);
  } 
  else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);

    if (wifiOK) lcd.write(0);
    else if (firstAttempt) {
      if (millis() - blinkTime > 500) {
        blinkState = !blinkState;
        blinkTime = millis();
      }
      lcd.write(blinkState ? 0 : ' ');
    }
    else lcd.write(1);

    lcd.setCursor(11,1);
    lcd.print(currentTime);
  }
}
