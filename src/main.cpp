#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <fauxmoESP.h>

// ---------- Global Objects ----------
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
fauxmoESP fauxmo;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST = UTC+5:30

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------- Pins ----------
const int sensor1 = 14;  // D5
const int sensor2 = 12;  // D6
const int sensor3 = 13;  // D7
const int sensor4 = 4;   // D2
const int relayPin = 16; // D0
const int manualPin = 2; // D4

// ---------- WiFi Icons ----------
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// ---------- Global State ----------
bool wifiConnected = false;
bool apActive = false;
unsigned long apStartMillis = 0;
unsigned long blinkTicker = 0;
bool blinkState = false;

bool motorON = false;
bool lastMotorState = false;
unsigned long motorTime = 0;

bool timeSynced = false;
unsigned long rtcOffsetSeconds = 0;
unsigned long rtcLastSyncMillis = 0;

bool lastConnected = false;

// CRITICAL FIX: Save credentials when they are valid!
String savedSSID = "";
String savedPASS = "";

const unsigned long TRY_SAVED_MS   = 15000UL;
const unsigned long AP_TOTAL_MS    = 180000UL;  // 3 minutes AP timeout
const unsigned long AP_SCROLL_MS   = 30000UL;

// ---------- Helper Functions ----------
void formatHHMM(unsigned long totalSeconds, char *out) {
  unsigned long h = (totalSeconds / 3600UL) % 24;
  unsigned long m = (totalSeconds / 60UL) % 60;
  sprintf(out, "%02lu:%02lu", h, m);
}

void getCurrentHHMM(char *out) {
  if (WiFi.status() == WL_CONNECTED && timeClient.update()) {
    String t = timeClient.getFormattedTime();
    t.substring(0,5).toCharArray(out, 6);
    return;
  }
  if (timeSynced) {
    unsigned long elapsed = (millis() - rtcLastSyncMillis) / 1000UL;
    unsigned long total = rtcOffsetSeconds + elapsed;
    formatHHMM(total, out);
    return;
  }
  strcpy(out, "--:--");
}

void lcdShowWifi(bool connected) {
  lcd.setCursor(10,1);
  lcd.write((uint8_t)(connected ? 0 : 1));
}

// ---------- AP Mode Web Pages ----------
String htmlFormPage(const String &msg = "") {
  String s = "<!doctype html><html><head><meta name='viewport' content='width=device-width'></head><body style='font-family:Arial;margin:40px'>";
  s += "<h3>KBC Water Tank Setup</h3>";
  if (msg.length()) s += "<p style='color:red'><b>" + msg + "</b></p>";
  s += "<form method='POST' action='/save'>";
  s += "SSID:<br><input name='ssid' required><br><br>";
  s += "Password:<br><input type='password' name='pwd'><br><br>";
  s += "<input type='submit' value='Save & Connect' style='padding:10px 20px;font-size:16px'>";
  s += "</form><hr><small>AP: KBC-Setup | Pass: 12345678</small></body></html>";
  return s;
}

void handleRoot() { server.send(200, "text/html", htmlFormPage()); }

void handleSave() {
  String ssid = server.arg("ssid");
  String pwd  = server.arg("pwd");

  if (ssid.length() == 0) {
    server.send(200, "text/html", htmlFormPage("SSID cannot be empty!"));
    return;
  }

  server.send(200, "text/html", "<html><body><h3>Connecting to " + ssid + "...</h3><p>Device will restart if successful.</p></body></html>");

  WiFi.persistent(true);
  WiFi.begin(ssid.c_str(), pwd.c_str());
  delay(500);
  ESP.restart();
}

// ---------- AP Control ----------
void startCustomAP() {
  if (apActive) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("KBC-Setup", "12345678");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  apActive = true;
  apStartMillis = millis();
  blinkTicker = millis();
}

void stopCustomAP() {
  if (!apActive) return;
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);  // Important!
  apActive = false;
}

// ---------- WiFi Connection ----------
bool trySavedWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);  // Very important!
  WiFi.begin();

  unsigned long start = millis();
  while (millis() - start < TRY_SAVED_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;

      // SAVE CREDENTIALS WHILE THEY ARE VALID!
      savedSSID = WiFi.SSID();
      savedPASS = WiFi.psk();

      Serial.print("Connected! IP: ");
      Serial.println(WiFi.localIP());
      Serial.println("Saved SSID: " + savedSSID);

      timeClient.begin();
      if (timeClient.update()) {
        timeSynced = true;
        rtcOffsetSeconds = timeClient.getHours()*3600UL +
                           timeClient.getMinutes()*60UL +
                           timeClient.getSeconds();
        rtcLastSyncMillis = millis();
      }
      lastConnected = true;
      return true;
    }
    delay(200);
  }

  wifiConnected = false;
  lastConnected = false;
  return false;
}

// ---------- OTA ----------
void setupWebOTA() {
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
}

// ---------- Fauxmo (Alexa) ----------
void wemoCallback(unsigned char device_id, const char *device_name, bool state, unsigned char value) {
  motorON = state;
  if (motorON && !lastMotorState) motorTime = millis();
  Serial.printf("Alexa: Motor %s\n", state ? "ON" : "OFF");
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(manualPin, INPUT_PULLUP);
  digitalWrite(relayPin, LOW);

  Wire.begin(0, 5);  // SDA=D1=0, SCL=D2=5 on NodeMCU
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.clear();
  lcd.setCursor(4,0); lcd.print("KB.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(1500);

  if (!trySavedWifi()) {
    startCustomAP();
  }

  setupWebOTA();

  fauxmo.createServer(true);
  fauxmo.setPort(80);
  fauxmo.addDevice("Motor");
  fauxmo.onSetState(wemoCallback);
  fauxmo.enable(true);
}

// ---------- Main Loop ----------
void loop() {
  server.handleClient();
  fauxmo.handle();

  // ---------- Read Sensors ----------
  bool s1 = digitalRead(sensor1) == LOW;
  bool s2 = digitalRead(sensor2) == LOW;
  bool s3 = digitalRead(sensor3) == LOW;
  bool s4 = digitalRead(sensor4) == LOW;

  String level = "0%";
  if (s4 && s3 && s2 && s1) level = "100%";
  else if (s3 && s2 && s1)  level = "75%";
  else if (s2 && s1)        level = "50%";
  else if (s1)              level = "25%";

  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // ---------- AP MODE ----------
  if (apActive) {
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(12,0); lcd.print("    ");
    lcd.setCursor(12,0); lcd.print(level);

    // Blinking WiFi icon
    if (millis() - blinkTicker >= 500) {
      blinkTicker = millis();
      blinkState = !blinkState;
      lcd.setCursor(10,1);
      lcd.write(blinkState ? 0 : 32);  // 32 = space
    }

    // Auto exit AP if WiFi connects
    if (isConnected && savedSSID.length() > 0) {
      stopCustomAP();
      delay(500);
      ESP.restart();
    }

    // AP timeout
    if (millis() - apStartMillis > AP_TOTAL_MS) {
      stopCustomAP();
    }
    delay(200);
    return;
  }

  // ---------- WiFi Reconnection (THE FIX!) ----------
  if (!isConnected && savedSSID.length() > 0) {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 10000) {
      lastReconnectAttempt = millis();
      Serial.print("WiFi lost! Reconnecting to: ");
      Serial.println(savedSSID);
      WiFi.disconnect();
      delay(100);
      WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
    }
  }

  // Fresh reconnect detected
  if (isConnected && !lastConnected) {
    Serial.println("WiFi Reconnected!");
    savedSSID = WiFi.SSID();  // Refresh just in case
    savedPASS = WiFi.psk();

    timeClient.begin();
    if (timeClient.update()) {
      timeSynced = true;
      rtcOffsetSeconds = timeClient.getHours()*3600UL +
                         timeClient.getMinutes()*60UL +
                         timeClient.getSeconds();
      rtcLastSyncMillis = millis();
    }
  }
  lastConnected = isConnected;

  // ---------- Manual Button ----------
  static unsigned long lastPress = 0;
  if (digitalRead(manualPin) == LOW && millis() - lastPress > 400) {
    motorON = !motorON;
    if (motorON) motorTime = millis();
    lastPress = millis();
    delay(200);
  }

  // ---------- Auto Motor Logic ----------
  if (level == "0%")  motorON = true;
  if (level == "100%") motorON = false;

  if (motorON && !lastMotorState) motorTime = millis();
  lastMotorState = motorON;
  digitalWrite(relayPin, motorON ? HIGH : LOW);

  // ---------- LCD Update ----------
  lcd.setCursor(0,0);
  lcd.print("Water Level:");
  lcd.setCursor(12,0); lcd.print("    ");
  lcd.setCursor(12,0); lcd.print(level);

  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[17];
    sprintf(buf, "Motor:ON %02dM  ", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    char hhmm[6];
    getCurrentHHMM(hhmm);
    lcd.setCursor(11,1); lcd.print(hhmm);
  }

  lcdShowWifi(isConnected);

  delay(200);
}
