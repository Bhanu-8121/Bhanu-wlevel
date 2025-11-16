/* FINAL KBC WATER LEVEL CONTROLLER
   ------------------------------------
   ✓ WiFi ICON logic EXACT per requirement
   ✓ Auto-Reconnect every 30 seconds
   ✓ Water level always updates (even in AP mode)
   ✓ Time HH:MM shown at line2 col 11..15
   ✓ WiFi icon at line2 col10
   ✓ AP mode scroll + blinking 0.5s
   ✓ Saved WiFi trial 15s with blinking 1s
   ✓ New credentials saved → reboot
*/

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <fauxmoESP.h>

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
fauxmoESP fauxmo;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ----- Pins -----
const int sensor1 = 14;
const int sensor2 = 12;
const int sensor3 = 13;
const int sensor4 = 4;
const int relayPin = 16;
const int manualPin = 2;

// ----- WiFi Symbols -----
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// ----- State -----
bool wifiConnected = false;
bool apActive = false;
unsigned long apStartMillis = 0;

bool motorON = false;
bool lastMotorState = false;
unsigned long motorTime = 0;

// Soft RTC
bool timeSynced = false;
unsigned long rtcOffsetSeconds = 0;
unsigned long rtcLastSyncMillis = 0;

// Intervals
const unsigned long TRY_SAVED_MS = 15000UL;
const unsigned long AP_TOTAL_MS  = 120000UL;
const unsigned long AP_SCROLL_MS = 30000UL;

// Blink
unsigned long blinkTicker = 0;
bool blinkState = false;

// Format HH:MM
void formatHHMM(unsigned long sec, char *out) {
  unsigned long h = (sec / 3600UL) % 24;
  unsigned long m = (sec / 60UL) % 60;
  sprintf(out, "%02lu:%02lu", h, m);
}

// OTA
void setupWebOTA() {
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
}

// Alexa callback
void wemoCallback(unsigned char id, const char *name, bool state, unsigned char value) {
  motorON = state;
  if (motorON && !lastMotorState) motorTime = millis();
}

// HTML page
String htmlFormPage() {
  String s = "<html><body><h3>KBC Setup</h3>";
  s += "<form method='POST' action='/save'>";
  s += "SSID:<br><input name='ssid'><br>Password:<br>";
  s += "<input name='pwd' type='password'><br><br>";
  s += "<input type='submit'></form></body></html>";
  return s;
}

void handleRoot() {
  server.send(200, "text/html", htmlFormPage());
}

void handleSave() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "Missing SSID");
    return;
  }
  String ssid = server.arg("ssid");
  String pwd  = server.arg("pwd");

  WiFi.persistent(true);
  WiFi.begin(ssid.c_str(), pwd.c_str());

  server.send(200, "text/html",
  "<html><body><h3>Connecting… Rebooting if successful.</h3></body></html>");
}

// Start AP
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
  blinkState = false;
}

void stopCustomAP() {
  if (!apActive) return;
  server.stop();
  WiFi.softAPdisconnect(true);
  apActive = false;
}

// Try saved WiFi with 1s blink
bool trySavedWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin();

  blinkTicker = millis();

  unsigned long start = millis();
  while (millis() - start < TRY_SAVED_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;

      timeClient.begin();
      if (timeClient.update()) {
        timeSynced = true;
        rtcOffsetSeconds =
          timeClient.getHours()*3600UL +
          timeClient.getMinutes()*60UL +
          timeClient.getSeconds();
        rtcLastSyncMillis = millis();
      }
      return true;
    }

    // blink 1s
    if (millis() - blinkTicker >= 1000UL) {
      blinkTicker = millis();
      blinkState = !blinkState;
      lcd.setCursor(10,1);
      if (blinkState) lcd.write((uint8_t)0);
      else lcd.print(" ");
    }

    delay(100);
  }
  WiFi.disconnect(true);
  wifiConnected = false;
  return false;
}

// Get HH:MM
void getCurrentHHMM(char *out) {
  if (timeSynced && WiFi.status() == WL_CONNECTED) {
    String t = timeClient.getFormattedTime();
    t.substring(0,5).toCharArray(out,6);
    return;
  }
  if (timeSynced) {
    unsigned long elapsed = (millis() - rtcLastSyncMillis)/1000UL;
    formatHHMM(rtcOffsetSeconds + elapsed, out);
    return;
  }
  strcpy(out,"--:--");
}

void setup() {
  Serial.begin(115200);

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(manualPin, INPUT_PULLUP);
  digitalWrite(relayPin, LOW);

  Wire.begin(0,5);
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  // Splash
  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000);
  lcd.clear();

  // Initial display
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write((uint8_t)1);
  lcd.setCursor(11,1); lcd.print("--:--");

  if (!trySavedWifi())
    startCustomAP();

  setupWebOTA();
  fauxmo.createServer(true);
  fauxmo.setPort(80);
  fauxmo.addDevice("Motor");
  fauxmo.onSetState(wemoCallback);
  fauxmo.enable(true, true);
}

void loop() {
  server.handleClient();
  fauxmo.handle();

  // -------- SENSOR READ ALWAYS --------
  bool s1=false,s2=false,s3=false,s4=false;
  int s4count=0;
  for(int i=0;i<7;i++){
    if(digitalRead(sensor1)==LOW) s1=true;
    if(digitalRead(sensor2)==LOW) s2=true;
    if(digitalRead(sensor3)==LOW) s3=true;
    if(digitalRead(sensor4)==LOW) s4count++;
    delay(1);
  }
  s4 = (s4count >= 5);

  String level;
  if (s4&&s3&&s2&&s1) level="100%";
  else if (s3&&s2&&s1) level="75%";
  else if (s2&&s1) level="50%";
  else if (s1) level="25%";
  else level="0%";

  // --- WATER LEVEL ALWAYS UPDATED ---
  lcd.setCursor(0,0);
  lcd.print("Water Level:");
  lcd.setCursor(12,0); lcd.print("    ");
  lcd.setCursor(12,0); lcd.print(level);

  // -------- AP MODE --------
  if (apActive) {
    unsigned long now = millis();

    // blink 0.5s
    if (now - blinkTicker >= 500UL) {
      blinkTicker = now;
      blinkState = !blinkState;
      lcd.setCursor(10,1);
      if (blinkState) lcd.write((uint8_t)0);
      else lcd.print(" ");
    }

    // scroll after 30s
    if (now - apStartMillis >= AP_SCROLL_MS) {
      static unsigned long lastScroll=0;
      static int offset=0;

      String msg="Connect to KBC-Setup and select your WiFi";
      if (now - lastScroll >=300) {
        lastScroll = now;
        String part="";
        for(int i=0;i<16;i++) {
          int idx = offset+i;
          if(idx < msg.length()) part += msg[idx];
          else part += ' ';
        }
        lcd.setCursor(0,1);
        lcd.print(part);
        offset++;
        if(offset > msg.length()) offset=0;
      }
    }

    // AP save success → reboot
    if (WiFi.status()==WL_CONNECTED) {
      stopCustomAP();
      delay(300);
      ESP.restart();
    }

    // AP timeout
    if (millis() - apStartMillis >= AP_TOTAL_MS) {
      stopCustomAP();
    }

    return;
  }

  // -------- NORMAL MODE --------
  bool isConnected = (WiFi.status()==WL_CONNECTED);

  // Auto-reconnect every 30s
  static unsigned long lastReconnectTry=0;
  if (!isConnected && millis()-lastReconnectTry>=30000){
    WiFi.begin();
    lastReconnectTry = millis();
  }

  if (isConnected) {
    if (timeClient.update()) {
      timeSynced=true;
      rtcOffsetSeconds =
        timeClient.getHours()*3600UL +
        timeClient.getMinutes()*60UL +
        timeClient.getSeconds();
      rtcLastSyncMillis=millis();
    }
  }

  // Manual button
  static unsigned long lastPress=0;
  if (digitalRead(manualPin)==LOW) {
    if(millis()-lastPress>300){
      motorON=!motorON;
      if(motorON) motorTime=millis();
      lastPress=millis();
      delay(200);
    }
  }

  // Safety
  if (level=="0%") motorON=true;
  if (level=="100%") motorON=false;

  if (motorON && !lastMotorState) motorTime=millis();
  lastMotorState = motorON;
  digitalWrite(relayPin, motorON ? HIGH : LOW);

  // ----- LCD line2 -----
  lcd.setCursor(0,1);

  if (motorON) {
    int mins = (millis()-motorTime) / 60000;
    char buf[17];
    sprintf(buf,"Motor:ON %02dM", mins);
    lcd.print(buf);

    lcd.setCursor(10,1);
    if (isConnected) lcd.write((uint8_t)0);
    else lcd.write((uint8_t)1);

  } else {
    lcd.print("Motor:OFF ");

    char hhmm[6];
    getCurrentHHMM(hhmm);

    lcd.setCursor(11,1);
    lcd.print(hhmm);

    lcd.setCursor(10,1);
    if (isConnected) lcd.write((uint8_t)0);
    else lcd.write((uint8_t)1);
  }

  delay(150);
}
