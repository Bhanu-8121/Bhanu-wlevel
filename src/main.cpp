/* Water motor Alexa - merged sketch
   Merges user's main code with Espalexa-based Alexa local integration
   - OTA webserver moved to port 81 (so Alexa can use port 80)
   - Manual switch on D4 (GPIO2) using INPUT_PULLUP
   - Espalexa device name: "Water Motor"
   - Protection: if level == "100%" then ON requests (Alexa or switch) are blocked and device remains OFF
   - Alexa state sync and LCD updates included
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>

// ===== Alexa (Espalexa) =====
#include <Espalexa.h>
Espalexa espalexa; // object to manage alexa devices

// ===== WEB OTA =====
// NOTE: Port changed to 81 to allow Alexa library to use port 80
ESP8266WebServer server(81);
ESP8266HTTPUpdateServer httpUpdater;

// ===== WIFI =====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST

LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Symbols
byte wifiOn[8] = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14; // D5
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 4;  // D2
const int relayPin = 16; // D0

// <<< ADDED: Manual Switch >>>
const int switchPin = 2; // D4 (GPIO2) - connected to GND when pressed (use INPUT_PULLUP)

// WiFi state
bool wifiOK = false;

// WiFi Manager State
unsigned long connectStartMillis = 0;
bool apModeLaunched = false; // Flag to ensure AP only runs once

bool motorON = false;
unsigned long motorTime = 0;

// Local RTC
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

// BLINK variables
unsigned long blinkTicker = 0;
bool blinkState = false;

// Keep last switch state for edge detect
static int lastSwitchState = HIGH;

// Global water level state
String globalLevel = "0%";

// Forward declarations
void requestMotorOn(String source, String level);
void requestMotorOff(String source);

// ========= Alexa / Espalexa callback ==========
void alexaCallback(uint8_t device_id, bool value) {
  String level = globalLevel;

  if (value) { // Alexa requests ON
    if (level == "100%") {
      Serial.println("Alexa requested ON but tank full → blocked");
      motorON = false;
      digitalWrite(relayPin, LOW);

      espalexa.setDeviceState(0, false);  // Keep Alexa OFF
    } else {
      requestMotorOn("Alexa", level);
      espalexa.setDeviceState(0, true);
    }
  }  
  else { // Alexa requests OFF
    requestMotorOff("Alexa");
    espalexa.setDeviceState(0, false);
  }
}

// SAFE MOTOR ON
void requestMotorOn(String source, String level) {

  if (level == "100%") {
    motorON = false;
    digitalWrite(relayPin, LOW);
    Serial.println("Tank full → Motor BLOCKED (from: " + source + ")");
    espalexa.setDeviceState(0, false);
    return;
  }

  motorON = true;
  digitalWrite(relayPin, HIGH);
  motorTime = millis();

  Serial.println("Motor turned ON by " + source);
  espalexa.setDeviceState(0, true);
}

// SAFE MOTOR OFF
void requestMotorOff(String source) {
  motorON = false;
  digitalWrite(relayPin, LOW);

  Serial.println("Motor turned OFF by " + source);
  espalexa.setDeviceState(0, false);
}

// ===== Alexa setup =====
void setupAlexa() {
  espalexa.addDevice("Water Motor", alexaCallback);
  espalexa.begin();
  Serial.println("Espalexa started - device: Water Motor");
}

void setup() {
  Serial.begin(115200);

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  // SWITCH
  pinMode(switchPin, INPUT_PULLUP);

  Wire.begin(0, 5);
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000); lcd.clear();

  // Offline screen
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(1);
  lcd.setCursor(11,1); lcd.print("--:--");

  WiFi.setAutoReconnect(true);
  WiFi.begin();
  connectStartMillis = millis();

  setupAlexa();
}

void loop() {

  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // WiFiManager AP logic
  if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.startConfigPortal("KBC-Setup", "12345678");

    apModeLaunched = true;
    isConnected = (WiFi.status() == WL_CONNECTED);
  }

  if (isConnected)
    server.handleClient(); // OTA on port 81

  espalexa.loop();

  // When WiFi connects first time
  if (isConnected && !wifiOK) {
    wifiOK = true;
    timeClient.begin();
    httpUpdater.setup(&server, "/update", "kbc", "987654321");
    server.begin();

    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(0,1); lcd.print("WiFi Connected");
    delay(1500);

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }

  if (!isConnected && wifiOK) {
    wifiOK = false;
    lcd.setCursor(0,1); lcd.print("WiFi Disconnect ");
    delay(1500);
  }

  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
  }

  String currentTime = "--:--";
  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis)/1000;
    unsigned long total = offsetSeconds + elapsed;
    int h = (total/3600) % 24;
    int m = (total/60) % 60;
    char buf[6];
    sprintf(buf, "%02d:%02d", h, m);
    currentTime = String(buf);
  }

  // Sensor debounce
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

  globalLevel = level;

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(12,0); lcd.print(level);

  // ===== Auto System Logic =====
  if (level=="0%" && !motorON){
    requestMotorOn("System", level);
  }
  if (level=="100%" && motorON){
    requestMotorOff("System");
  }

  // ===== Manual Switch Logic =====
  int currentSwitch = digitalRead(switchPin);
  if (lastSwitchState == HIGH && currentSwitch == LOW) {
    if (motorON)
      requestMotorOff("Switch");
    else
      requestMotorOn("Switch", level);

    delay(50);
  }
  lastSwitchState = currentSwitch;

  // ===== LCD Update =====
  lcd.setCursor(0,1);
  if (motorON){
    int mins=(millis()-motorTime)/60000;
    char buf[17];
    sprintf(buf,"Motor:ON %02dM",mins);
    lcd.print(buf);
  }
  else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);
    if (wifiOK) lcd.write(0);
    else lcd.write(1);
  }

  lcd.setCursor(11,1); lcd.print(currentTime);

  delay(200);
}
