#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <Espalexa.h>

// ==== Alexa ====
Espalexa espalexa;

// ==== OTA on port 81 ====
ESP8266WebServer otaServer(81);
ESP8266HTTPUpdateServer httpUpdater;

// ==== WEB SERIAL MONITOR on port 82 ====
ESP8266WebServer logServer(82);
String serialBuffer = "";

// Log function
void addLog(String msg) {
  Serial.println(msg);
  serialBuffer += msg + "\n";
  if (serialBuffer.length() > 8000) serialBuffer.remove(0, 3000);
}

// ==== NTP Time ====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

// ==== LCD ====
LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Icons
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14;  // D5
const int sensor2 = 12;  // D6
const int sensor3 = 13;  // D7
const int sensor4 = 4;   // D2
const int relayPin = 16; // D0
const int switchPin = 2; // D4 (10k pull-up resistor lagana compulsory!)

bool wifiOK = false;
unsigned long connectStartMillis = 0;
bool apModeLaunched = false;

bool motorON = false;
unsigned long motorTime = 0;

bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

unsigned long blinkTicker = 0;
bool blinkState = false;

String globalLevel = "0%";
int lastSwitchState = HIGH;

// ========================================
//           MOTOR CONTROL LOGIC
// ========================================
void requestMotorOn(String source, String level) {
  if (level == "100%") {
    motorON = false;
    digitalWrite(relayPin, LOW);
    addLog("BLOCKED: Tank full -> ON rejected (" + source + ")");
    return;
  }
  motorON = true;
  digitalWrite(relayPin, HIGH);
  motorTime = millis();
  addLog("Motor ON by " + source);
}

void requestMotorOff(String source) {
  motorON = false;
  digitalWrite(relayPin, LOW);
  addLog("Motor OFF by " + source);
}

// ========================================
//              ALEXA CALLBACK
// ========================================
void alexaCallback(uint8_t brightness) {
  if (brightness == 0) {
    requestMotorOff("Alexa");
  } else {
    if (globalLevel == "100%") {
      motorON = false;
      digitalWrite(relayPin, LOW);
      addLog("Alexa tried ON -> BLOCKED (full tank)");
    } else {
      requestMotorOn("Alexa", globalLevel);
    }
  }
}

// ========================================
//               SETUP
// ========================================
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  pinMode(switchPin, INPUT_PULLUP);  // D4 - 10k external pull-up compulsory!

  Wire.begin(0, 5);  // SDA=D2, SCL=D1
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000);
  lcd.clear();

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(1);
  lcd.setCursor(11,1); lcd.print("--:--");

  WiFiManager wm;
  wm.setAPCallback([](WiFiManager *wm) {
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Connect to:");
    lcd.setCursor(0,1); lcd.print("KBC-Setup");
    addLog("AP Mode: KBC-Setup");
  });
  wm.setConfigPortalTimeout(180);

  addLog("Booting... Connecting WiFi");

  if (!wm.autoConnect("KBC-Setup", "12345678")) {
    addLog("WiFi setup failed -> Restart");
    lcd.clear(); lcd.print("WiFi Failed!"); delay(3000);
    ESP.restart();
  }

  wifiOK = true;
  addLog("WiFi Connected: " + WiFi.localIP().toString());

  timeClient.begin();

  // Start services
  httpUpdater.setup(&otaServer, "/update", "kbc", "987654321");
  otaServer.begin();
  addLog("OTA Ready: http://" + WiFi.localIP().toString() + ":81/update");

  logServer.on("/log", HTTP_GET, []() {
    logServer.send(200, "text/plain", serialBuffer);
  });
  logServer.begin();
  addLog("Web Log: http://" + WiFi.localIP().toString() + ":82/log");

  espalexa.addDevice("Water Motor", alexaCallback);
  espalexa.begin();

  addLog("Alexa Ready - Say: Alexa, discover devices");

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(0);
}

// ========================================
//                 LOOP
// ========================================
void loop() {
  otaServer.handleClient();
  logServer.handleClient();
  espalexa.loop();

  // NTP sync
  if (timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
  }

  String currentTime = "--:--";
  if (timeSynced) {
    unsigned long total = offsetSeconds + (millis() - lastSyncMillis)/1000;
    char t[6]; sprintf(t, "%02d:%02d", (total/3600)%24, (total/60)%60);
    currentTime = t;
  }

  // Read sensors (debounce)
  bool s1=false, s2=false, s3=false; int s4c=0;
  for(int i=0; i<7; i++) {
    if(digitalRead(sensor1)==LOW) s1=true;
    if(digitalRead(sensor2)==LOW) s2=true;
    if(digitalRead(sensor3)==LOW) s3=true;
    if(digitalRead(sensor4)==LOW) s4c++;
    delay(10);
  }
  bool s4 = (s4c >= 5);

  String level = s4&&s3&&s2&&s1 ? "100%" :
                 s3&&s2&&s1 ? "75%" :
                 s2&&s1 ? "50%" :
                 s1 ? "25%" : "0%";
  globalLevel = level;

  lcd.setCursor(12,0); lcd.print("    ");
  lcd.setCursor(12,0); lcd.print(level);

  // Auto control
  if (level == "0%" && !motorON) requestMotorOn("Auto", level);
  if (level == "100%" && motorON) requestMotorOff("Auto");

  // Manual switch
  int sw = digitalRead(switchPin);
  if (lastSwitchState == HIGH && sw == LOW) {
    motorON ? requestMotorOff("Switch") : requestMotorOn("Switch", level);
    delay(80);
  }
  lastSwitchState = sw;

  // Display
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[17]; sprintf(buf, "Motor:ON %02dM  ", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);
    if (wifiOK) {
      lcd.write(0);
    } else {
      if (!apModeLaunched) {
        if (millis() - blinkTicker >= 500) {
          blinkTicker = millis();
          blinkState = !blinkState;
        }
        lcd.print(blinkState ? "WiFi" : "    ");
      } else {
        lcd.write(1);
      }
    }
    lcd.setCursor(11,1); lcd.print(currentTime);
  }

  delay(200);
}
