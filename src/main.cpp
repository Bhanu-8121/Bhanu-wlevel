
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <Espalexa.h>
String lastLoggedLevel = "";

// ==== Alexa ====
Espalexa espalexa;
bool alexaStarted = false; // ensure espalexa.begin() only once after WiFi connect

// ==== OTA on port 81 ====
ESP8266WebServer server(81);
ESP8266HTTPUpdateServer httpUpdater;

// ==== WEB SERIAL MONITOR on port 82 ====
ESP8266WebServer logServer(82);
String serialBuffer = "";

// Function to record logs (with timestamp)
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800);

void addLog(String msg) {
  // 1. Get the current calendar date and time from the NTP Client
  time_t rawtime = timeClient.getEpochTime();
  struct tm * ti;
  ti = localtime(&rawtime);

  // 2. Format as DD-MM-YYYY HH:MM:SS
  char dateBuffer[25];
  sprintf(dateBuffer, "%02d-%02d-%04d %02d:%02d:%02d", 
          ti->tm_mday, 
          ti->tm_mon + 1, 
          ti->tm_year + 1900, 
          ti->tm_hour, 
          ti->tm_min, 
          ti->tm_sec);

  // 3. Create the log line with the new date format
  String line = "[" + String(dateBuffer) + "] " + msg;
  
  // Serial.println(line); // DO NOT UNCOMMENT - Keeps RX pin safe for switch
  
  // 4. Add to the web buffer
  serialBuffer += line + "\n";
  
  // 5. Keep buffer from getting too large (reduced to 5000 for better stability)
  if (serialBuffer.length() > 5000) {
    serialBuffer = serialBuffer.substring(serialBuffer.indexOf('\n', 500) + 1);
  }
}

// ==== WIFI / TIME ====
//WiFiUDP ntpUDP;
//NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST offset

LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Icons
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14; // D5
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 5;  // D1  moved from d2
const int relayPin = 16; // D0
const int switchPin = 3; // rx (moved here to fress up d4 for lcd)
// NOTE: You told "Use D4" for push button. On NodeMCU D4 is GPIO2. Above we already set switchPin=2 to match D4.

// WiFi state
bool wifiOK = false;

// WiFi Manager State
unsigned long connectStartMillis = 0;
bool apModeLaunched = false; // AP only runs once per boot

// Motor state
bool motorON = false;
unsigned long motorTime = 0;

String globalLevel = "0%";
int lastSwitchState = HIGH;

// Blink variables
unsigned long blinkTicker = 0;
bool blinkState = false;

// =========================================
//        MOTOR CONTROL SAFE LOGIC
// =========================================
void requestMotorOn(String source, String level)
{
  if (level == "100%") {
    motorON = false;
    digitalWrite(relayPin, LOW);
    addLog("BLOCKED: Tank full → ON rejected (" + source + ")");
    return;
  }
  motorON = true;
  digitalWrite(relayPin, HIGH);
  motorTime = millis();
  addLog("Motor ON by " + source);
}

void requestMotorOff(String source)
{
  motorON = false;
  digitalWrite(relayPin, LOW);
  addLog("Motor OFF by " + source);
}

// =========================================
//              ALEXA CALLBACK
// =========================================
// Espalexa passes brightness (0–255), not bool
void alexaCallback(uint8_t brightness)
{
  String level = globalLevel;
  if (brightness == 0) {
    requestMotorOff("Alexa");
  } else {
    // brightness > 0 -> ON
    if (level == "100%") {
      motorON = false;
      digitalWrite(relayPin, LOW);
      addLog("Alexa tried ON → BLOCKED (full tank)");
    } else {
      requestMotorOn("Alexa", level);
    }
  }
}

void setupAlexa()
{
  espalexa.addDevice("Water Motor", alexaCallback);
  // do not call espalexa.begin() here — wait until WiFi connected so Espalexa's SSDP/UPnP works cleanly
  addLog("Alexa device registered (pending start on WiFi).");
}

// ===== WiFiManager Callback =====
void configModeCallback(WiFiManager *wm)
{
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Enter AP Mode");
  lcd.setCursor(0, 1); lcd.print("SSID:");
  lcd.setCursor(5, 1); lcd.print(wm->getConfigPortalSSID());
  addLog("Config Portal Started: " + wm->getConfigPortalSSID());
}

// ===== WEB OTA SETUP =====
void setupWebOTA()
{
  httpUpdater.setup(&server, "/update", "kbc", "987654321");
  server.begin();
  addLog("OTA Ready: http://" + WiFi.localIP().toString() + ":81/update");
}

// ===== WEB LOG SETUP =====
void setupWebLogServer()
{
  logServer.on("/log", HTTP_GET, []() {
    logServer.send(200, "text/plain", serialBuffer);
  });
  logServer.begin();
  addLog("Web Serial Log ready: http://" + WiFi.localIP().toString() + ":82/log");
}

// ================== SETUP ==================
void setup()
{
//  Serial.begin(115200); 
  addLog("Booting...");

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(switchPin, INPUT_PULLUP);

  digitalWrite(relayPin, LOW);

  Wire.begin(0,2);  //lcd d3 to SDA, d4 to scl
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(1500);
  lcd.clear();

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(1);
  lcd.setCursor(11,1); lcd.print("--:--");

  WiFi.setAutoReconnect(true);
  WiFi.begin(); // Start connecting non-blocking
  connectStartMillis = millis();

  // Register Alexa device (do NOT begin yet)
  setupAlexa();
}

// =========================================
//                   LOOP
// =========================================
void loop()
{
  // Connection state
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // --- WiFi/AP Manager Logic (runs only once after boot if not connected) ---
  if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {
    addLog("30s timeout. Launching Config Portal...");
    WiFiManager wm;
    wm.setAPCallback(configModeCallback);
    wm.setConfigPortalTimeout(180); // 3 minutes

    // startConfigPortal is blocking: shows captive portal and returns true if connected
    if (wm.startConfigPortal("KBC-Setup", "12345678")) {
      addLog("AP connection successful!");
    } else {
      addLog("AP timeout. Running offline.");
    }
    apModeLaunched = true; // ensure AP is attempted only once per boot
    // After exit, re-evaluate connection:
    isConnected = (WiFi.status() == WL_CONNECTED);
  }
  // --- End WiFi/AP Logic ---

  // handle servers/tests every loop
  if (isConnected) {
    server.handleClient();   // OTA (port 81)
  }
  logServer.handleClient();  // Log server (82) — safe to call even if not begun (it will error if not started; we'll only begin it after WiFi connect)
  // espalexa.loop() should be called after espalexa.begin() has been invoked
  if (alexaStarted) espalexa.loop();

  // Runs once when WiFi connects (or reconnects)

if (isConnected && !wifiOK) {
    wifiOK = true;

    // NTP
    timeClient.begin();

    // start OTA and logs
    setupWebOTA();
    setupWebLogServer();

    // Start Espalexa now that WiFi is available
    if (!alexaStarted) {
      espalexa.begin();
      alexaStarted = true;
      addLog("Espalexa started (Alexa discoverable).");
    }

    // After a successful connection, disable future AP attempts
    apModeLaunched = true;

    // show IP on LCD
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi Connected");
    lcd.setCursor(0,1);
    lcd.print(WiFi.localIP().toString());
    delay(2000);

    // return to normal screen
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(0,1); lcd.print("Motor:OFF ");
    lcd.setCursor(10,1); lcd.write(0);

    addLog("WiFi Connected: " + WiFi.localIP().toString());
  }


  // Runs once when WiFi disconnects
  if (!isConnected && wifiOK) {
    wifiOK = false;
    lcd.setCursor(0,1); lcd.print("WiFi Disconnect ");
    delay(1500);
    lcd.setCursor(0,1); lcd.print("Motor:OFF ");
    lcd.setCursor(10,1); lcd.write(1);
    addLog("WiFi Disconnected");
  }

  // --- Maintain NTP Time Sync ---
  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
  }

  // Build current time string
  String currentTime = "--:--";
  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis) / 1000;
    unsigned long total = offsetSeconds + elapsed;
    int h = (total / 3600) % 24;
    int m = (total / 60) % 60;
    char buf[6];
    sprintf(buf, "%02d:%02d", h, m);
    currentTime = String(buf);
  }

  // --- Sensor reading with de-bounce ---
  // --- New Sensor Reading (Simple & Fast) ---
  bool s1 = (digitalRead(sensor1) == LOW);
  bool s2 = (digitalRead(sensor2) == LOW);
  bool s3 = (digitalRead(sensor3) == LOW);
  bool s4 = (digitalRead(sensor4) == LOW);

  // --- New Level Logic (Priority Based) ---
  // We check from the top down. If the highest sensor is wet, that is the level.
  String level;
  if (s4)      level = "100%";
  else if (s3) level = "75%";
  else if (s2) level = "50%";
  else if (s1) level = "25%";
  else         level = "0%";

  globalLevel = level; // keep Alexa logic in sync

  // --- ADD THIS LOGIC STARTING AT LINE 337 for level updaet in log ---
  if (globalLevel != lastLoggedLevel) {
    addLog("Water Level: " + globalLevel);
    lastLoggedLevel = globalLevel; 
  }
  // --- END OF NEW log LOGIC ---
  
  // Update LCD - Level (top line)
  lcd.setCursor(0,0);
  lcd.print("Water Level:");
  lcd.setCursor(12,0);
  lcd.print("    "); // clear
  lcd.setCursor(12,0);
  lcd.print(level);

  // --- Auto Motor Logic with safety wrappers ---
  if (level == "0%" && !motorON) {
    requestMotorOn("System", level);
  }
  if (level == "100%" && motorON) {
    requestMotorOff("System");
  }

  // --- Manual switch (toggle on falling edge) ---
  int sw = digitalRead(switchPin);
  if (lastSwitchState == HIGH && sw == LOW) {
    if (motorON) requestMotorOff("Switch");
    else requestMotorOn("Switch", level);
    delay(80); // debounce
  }
  lastSwitchState = sw;

  // --- Bottom line: motor status, wifi icon, time ---
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[17]; // 16 chars + null
    sprintf(buf, "Motor:ON %02dM  ", mins);
    lcd.print(buf);
  } else {
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);
    if (wifiOK) {
      lcd.write(0); // wifiOn
    } else {
      if (!apModeLaunched) {
        if (millis() - blinkTicker >= 500) {
          blinkTicker = millis();
          blinkState = !blinkState;
        }
        if (blinkState) lcd.write((uint8_t)0);
        else lcd.print(" ");
      } else {
        lcd.write(1); // wifiOff
      }
    }
  }

  // Time on the right
  lcd.setCursor(11,1);
  lcd.print(currentTime);

  delay(200);
}
