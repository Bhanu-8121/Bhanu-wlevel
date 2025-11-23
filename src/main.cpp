/* Water motor Alexa - UPDATED with alexaServer
   - OTA server on port 81
   - Alexa HTTP server on port 80 (alexaServer) to properly respond to Echo discovery/commands
   - Manual switch on D4 (GPIO2)
   - Espalexa integration using getDevice(...)->setValue(...)
   - Tank 100% protection: block ON requests
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
Espalexa espalexa;

// Two web servers: alexaServer on port 80 for Alexa discovery/commands, server on port 81 for OTA
ESP8266WebServer alexaServer(80);
ESP8266WebServer server(81);
ESP8266HTTPUpdateServer httpUpdater;

// NTP / LCD
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
const int switchPin = 2; // D4 (to GND) - INPUT_PULLUP

// State variables
bool wifiOK = false;
bool apModeLaunched = false;
unsigned long connectStartMillis = 0;

bool motorON = false;
unsigned long motorTime = 0;

bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

unsigned long blinkTicker = 0;
bool blinkState = false;

String globalLevel = "0%";
int lastSwitchState = HIGH;

// Forward declarations
void requestMotorOn(String source, String level);
void requestMotorOff(String source);
void alexaServerSetup();
void setupWebOTA();
void configModeCallback(WiFiManager *myWiFiManager);

// ---------- Espalexa callback ----------
void alexaCallback(uint8_t device_id, bool state) {
  String level = globalLevel;

  if (state) { // Alexa requested ON
    if (level == "100%") {
      // Block ON at full tank
      motorON = false;
      digitalWrite(relayPin, LOW);
      if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(0);
      Serial.println("Alexa ON blocked: Tank full");
    } else {
      requestMotorOn("Alexa", level);
    }
  } else { // Alexa requested OFF
    requestMotorOff("Alexa");
  }
}

// ---------- Motor ON/OFF Control ----------
void requestMotorOn(String source, String level) {
  if (level == "100%") {
    motorON = false;
    digitalWrite(relayPin, LOW);
    if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(0);
    Serial.println("BLOCKED ON (tank full) from " + source);
    return;
  }
  motorON = true;
  digitalWrite(relayPin, HIGH);
  motorTime = millis();
  if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(255);
  Serial.println("Motor ON by " + source);
}

void requestMotorOff(String source) {
  motorON = false;
  digitalWrite(relayPin, LOW);
  if (espalexa.getDevice(0)) espalexa.getDevice(0)->setValue(0);
  Serial.println("Motor OFF by " + source);
}

// ---------- Alexa HTTP server setup ----------
void alexaServerSetup() {
  alexaServer.on("/", HTTP_GET, [](){
    if (!espalexa.handleAlexaApiCall(alexaServer.uri(), alexaServer.arg("state"))) {
      alexaServer.send(200, "text/plain", "");
    }
  });

  alexaServer.onNotFound([](){
    if (!espalexa.handleAlexaApiCall(alexaServer.uri(), alexaServer.arg("state"))) {
      alexaServer.send(404, "text/plain", "Not found");
    }
  });

  alexaServer.begin();
  Serial.println("Alexa server started on port 80");
}

// ---------- WiFiManager callback ----------
void configModeCallback(WiFiManager *myWiFiManager) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Enter AP Mode");
  lcd.setCursor(0, 1); lcd.print("SSID:");
  lcd.setCursor(5, 1); lcd.print(myWiFiManager->getConfigPortalSSID());
  Serial.println("Entered config mode: " + myWiFiManager->getConfigPortalSSID());
}

// ---------- OTA setup ----------
void setupWebOTA() {
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
  Serial.println("OTA ready on port 81");
  Serial.print("OTA URL: http://");
  Serial.print(WiFi.localIP());
  Serial.println(":81/update");
}

// ==============================
//            SETUP
// ==============================
void setup() {
  Serial.begin(115200);

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  pinMode(switchPin, INPUT_PULLUP);

  Wire.begin(0, 5);
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000); lcd.clear();

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(1);
  lcd.setCursor(11,1); lcd.print("--:--");

  WiFi.setAutoReconnect(true);
  WiFi.begin();
  connectStartMillis = millis();

  // Register Alexa device
  espalexa.addDevice("Water Motor", alexaCallback);
}

// ==============================
//             LOOP
// ==============================
void loop() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {
    Serial.println("Launching Config Portal...");
    WiFiManager wm;
    wm.setAPCallback(configModeCallback);
    wm.setConfigPortalTimeout(180);
    wm.startConfigPortal("KBC-Setup", "12345678");
    apModeLaunched = true;
    isConnected = (WiFi.status() == WL_CONNECTED);
  }

  if (isConnected) server.handleClient();

  if (isConnected && !wifiOK) {
    wifiOK = true;
    timeClient.begin();
    setupWebOTA();
    alexaServerSetup();
    espalexa.begin();

    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(0,1); lcd.print("WiFi Connected");
    delay(1500);
    lcd.setCursor(0,1); lcd.print(" ");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  }

  alexaServer.handleClient();
  espalexa.loop();

  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds =
      timeClient.getHours()*3600 +
      timeClient.getMinutes()*60 +
      timeClient.getSeconds();
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

  // Sensor reading
  bool s1=false, s2=false, s3=false;
  int s4c=0;
  for (int i=0;i<7;i++) {
    if (digitalRead(sensor1)==LOW) s1=true;
    if (digitalRead(sensor2)==LOW) s2=true;
    if (digitalRead(sensor3)==LOW) s3=true;
    if (digitalRead(sensor4)==LOW) s4c++;
    delay(10);
  }
  bool s4 = (s4c>=5);

  String level;
  if (s4&&s3&&s2&&s1) level="100%";
  else if (s3&&s2&&s1) level="75%";
  else if (s2&&s1) level="50%";
  else if (s1) level="25%";
  else level="0%";

  globalLevel = level;

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(12,0); lcd.print(level + " ");

  if (level=="0%" && !motorON) requestMotorOn("System", level);
  if (level=="100%" && motorON) requestMotorOff("System");

  int currentSwitch = digitalRead(switchPin);
  if (lastSwitchState == HIGH && currentSwitch == LOW) {
    if (motorON) requestMotorOff("Switch");
    else requestMotorOn("Switch", level);
    delay(80);
  }
  lastSwitchState = currentSwitch;

  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime)/60000;
    char buf[17];
    sprintf(buf, "Motor:ON %02dM", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);
    if (wifiOK) lcd.write(0);
    else lcd.write(1);
  }
  lcd.setCursor(11,1); lcd.print(currentTime);

  delay(200);
}
