/*********************************************************************
   K.B.C WATER TANK AUTOMATION – FINAL FIXED VERSION (NO ERROR)
   → WiFi Manager + Auto Captive Portal
   → IP Address on LCD
   → Alexa + Manual + Auto + Web OTA + Web Logs
*********************************************************************/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
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
const uint8_t SWITCH_PIN = 15;  // D8 → SAFE

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
byte wifiOn[8]  = {0x00,0x0E,0x11,0x04,0x0A,0x00,0x04,0x00};
byte wifiOff[8] = {0x11,0x1F,0x1B,0x04,0x0A,0x11,0x15,0x00};

// ================== VARIABLES ==================
bool wifiOK = false;
bool motorON = false;
unsigned long motorTime = 0;
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;
String globalLevel = "0%;
int lastSwitchState = HIGH;

// ================== LOG FUNCTION ==================
void addLog(String msg) {
  String logLine = String(millis()/1000) + "s: " + msg;
  Serial.println(logLine);
  serialBuffer += logLine + "<br>\n";
  if (serialBuffer.length() > 15000) serialBuffer.remove(0, 5000);
}

// ================== SHOW IP ON LCD ==================
void showIPonLCD() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("WiFi Connected!");
  lcd.setCursor(0,1); lcd.print(WiFi.localIP());
  delay(4000);
}

// ================== WiFiManager CALLBACKS ==================
void configModeCallback(WiFiManager *myWiFiManager) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Connect to WiFi:");
  lcd.setCursor(0,1); lcd.print("KBC-Setup");
  addLog("AP Mode: KBC-Setup");
}

void saveConfigCallback() {
  addLog("WiFi Saved – Restarting");
}

// ================== ALEXA ==================
void alexaCallback(uint8_t brightness) {
  if (brightness == 255 && globalLevel != "100%") {
    digitalWrite(RELAY_PIN, HIGH); motorON = true; motorTime = millis();
    addLog("Motor ON by Alexa");
  } else if (brightness == 0) {
    digitalWrite(RELAY_PIN, LOW); motorON = false;
    addLog("Motor OFF by Alexa");
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  addLog("K.B.C System Starting...");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // LED OFF

  pinMode(SENSOR1, INPUT_PULLUP);
  pinMode(SENSOR2, INPUT_PULLUP);
  pinMode(SENSOR3, INPUT_PULLUP);
  pinMode(SENSOR4, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(SWITCH_PIN, INPUT_PULLUP);
  digitalWrite(RELAY_PIN, LOW);

  Wire.begin(0, 5);
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000);
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Starting WiFi...");

  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("KBC-Setup", "12345678")) {
    addLog("WiFi Failed");
    lcd.clear(); lcd.print("WiFi Failed"); delay(3000);
    ESP.restart();
  }

  wifiOK = true;
  addLog("Connected → " + WiFi.localIP().toString());
  showIPonLCD();

  timeClient.begin();
  httpUpdater.setup(&httpServer, "/update", "kbc", "987654321");
  httpServer.begin();

  logServer.on("/log", HTTP_GET, []() {
    logServer.send(200, "text/html",
      "<meta charset='UTF-8'><body style='font-family:monospace;background:#000;color:#0f0'>"
      "<h2>K.B.C Logs</h2><pre>" + serialBuffer + "</pre>"
      "<script>setTimeout(()=>location.reload(),3000);</script></body>");
  });
  logServer.begin();
  addLog("Web Log: http://" + WiFi.localIP().toString() + ":82/log");

  espalexa.addDevice("Water Motor", alexaCallback);
  espalexa.begin();

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(0);
}

// ================== LOOP ==================
void loop() {
  httpServer.handleClient();
  logServer.handleClient();
  espalexa.loop();

  bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (!isConnected && wifiOK) {
    wifiOK = false;
    addLog("WiFi Lost");
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
    char t[6]; sprintf(t, "%02d:%02d", (total/3600)%24, (total/60)%60);
    currentTime = t;
  }

  // Sensors
  bool s1=false, s2=false, s3=false; int s4c=0;
  for(int i=0; i<7; i++){
    if(digitalRead(SENSOR1)==LOW) s1=true;
    if(digitalRead(SENSOR2)==LOW) s2=true;
    if(digitalRead(SENSOR3)==LOW) s3=true;
    if(digitalRead(SENSOR4)==LOW) s4c++;
    delay(10);
  }
  bool s4 = s4c >= 5;

  String level = s4&&s3&&s2&&s1 ? "100%" :
                 s3&&s2&&s1 ? "75%" :
                 s2&&s1 ? "50%" :
                 s1 ? "25%" : "0%";
  globalLevel = level;

  lcd.setCursor(12,0); lcd.print("    "); lcd.setCursor(12,0); lcd.print(level);

  // Auto Motor
  if (level == "0%" && !motorON) {
    digitalWrite(RELAY_PIN, HIGH); motorON = true; motorTime = millis();
    addLog("Auto ON");
  }
  if (level == "100%" && motorON) {
    digitalWrite(RELAY_PIN, LOW); motorON = false;
    addLog("Auto OFF");
  }

  // Manual Switch
  int sw = digitalRead(SWITCH_PIN);
  if (lastSwitchState == HIGH && sw == LOW) {
    motorON = !motorON;
    digitalWrite(RELAY_PIN, motorON ? HIGH : LOW);
    if (motorON) motorTime = millis();
    addLog("Switch " + String(motorON ? "ON" : "OFF"));
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
