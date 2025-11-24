/* Final stable merged code
   - WiFi try 30s -> AP mode (3 min) -> offline after timeout
   - Alexa on port 80 (Espalexa)
   - OTA on port 81
   - Web log on port 82
   - DNSServer added to improve captive portal reliability
   - Motor/switch/level logic preserved
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <Espalexa.h>

// --- Servers & helpers ---
ESP8266WebServer alexaServer(80);
Espalexa espalexa;

ESP8266WebServer otaServer(81);
ESP8266HTTPUpdateServer httpUpdater;

ESP8266WebServer logServer(82);
String serialBuffer = "";

// DNS server for captive portal
DNSServer dns;
const byte DNS_PORT = 53;

// NTP / Time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST

// LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14; // D5
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 4;  // D2
const int relayPin = 16; // D0
const int switchPin = 2; // D4 -> GND

// States
bool wifiOK = false;
bool apModeLaunched = false;    // ensures AP mode is started only once per boot
unsigned long connectStartMillis = 0;

bool motorON = false;
unsigned long motorTime = 0;

bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

String globalLevel = "0%";
int lastSwitchState = HIGH;

// debounce
unsigned long lastSwitchMillis = 0;
const unsigned long switchDebounceMs = 150UL;

// blink
unsigned long blinkTicker = 0;
bool blinkState = false;

// controls whether OTA/log servers have been started
bool otaStarted = false;
bool logStarted = false;

// logging helper
void addLog(const String &msg) {
  Serial.println(msg);
  serialBuffer += msg + "\n";
  if (serialBuffer.length() > 12000) serialBuffer.remove(0, 4000);
}

// Motor control (safe)
void requestMotorOn(const String &source, const String &level)
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

void requestMotorOff(const String &source)
{
  motorON = false;
  digitalWrite(relayPin, LOW);
  addLog("Motor OFF by " + source);
}

// Espalexa callback (brightness 0..255)
void alexaCallback(uint8_t brightness)
{
  if (brightness == 0) {
    requestMotorOff("Alexa");
  } else {
    if (globalLevel == "100%") {
      motorON = false;
      digitalWrite(relayPin, LOW);
      addLog("Alexa tried ON → BLOCKED (full tank)");
    } else {
      requestMotorOn("Alexa", globalLevel);
    }
  }
}

// Setup Espalexa with alexaServer (port 80) for discovery
void setupAlexa()
{
  espalexa.addDevice("Water Motor", alexaCallback);
  espalexa.begin(&alexaServer); // IMPORTANT: pass server for /description.xml etc.
  addLog("Alexa device added & Espalexa started");
}

// OTA setup (81)
void setupOTA()
{
  if (!otaStarted) {
    httpUpdater.setup(&otaServer, "/update", "kbc", "987654321");
    otaServer.begin();
    otaStarted = true;
    addLog("OTA Ready: http://" + WiFi.localIP().toString() + ":81/update");
  }
}

// Web log (82)
void setupWebLogServer()
{
  if (!logStarted) {
    logServer.on("/log", HTTP_GET, []() {
      logServer.send(200, "text/plain", serialBuffer);
    });
    logServer.begin();
    logStarted = true;
    addLog("Web Serial Log ready: http://" + WiFi.localIP().toString() + ":82/log");
  }
}

// WiFiManager AP callback (shows SSID on LCD)
void configModeCallback(WiFiManager *wm)
{
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Enter AP Mode");
  lcd.setCursor(0,1); lcd.print("SSID:");
  lcd.setCursor(5,1); lcd.print(wm->getConfigPortalSSID());
  addLog("Config Portal Started: " + wm->getConfigPortalSSID());
}

// Start Config Portal cleanly (blocking). This function will start the AP and run the portal.
// Returns true if WiFi connected by portal; false if timeout/no config.
bool startConfigPortalOnce()
{
  // Prepare WiFiManager
  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180); // 3 minutes

  // Start captive DNS (helps redirect on some devices)
  // startConfigPortal handles AP start and DNS internally, but we also start DNS to improve redirect reliability.
  // We first start a softAP quickly so softAPIP() is available for dns.start.
  WiFi.mode(WIFI_AP);
  WiFi.softAP("KBC-Setup", "12345678");
  IPAddress apIP = WiFi.softAPIP();
  delay(200); // give AP short time to initialize

  // Start DNS wildcard for captive portal (redirect all names to AP IP)
  dns.start(DNS_PORT, "*", apIP);
  addLog("DNSServer started for captive portal at " + apIP.toString());

  // Now run the blocking config portal (it will start its own AP if necessary; but we already have one)
  bool res = wm.startConfigPortal("KBC-Setup", "12345678"); // blocks until configured or timeout

  // stop DNS server after portal finishes (either success or timeout)
  dns.stop();
  addLog(String("Config portal returned: ") + (res ? "connected" : "timeout/failed"));

  // If returned true, WiFiManager already connected to new WiFi.
  return res;
}

void updateLCDTop(const String &level)
{
  static String prevLevel = "";
  if (level != prevLevel) {
    lcd.setCursor(0,0);
    lcd.print("Water Level:");
    lcd.setCursor(12,0);
    lcd.print("    "); // clear area
    lcd.setCursor(12,0);
    lcd.print(level);
    prevLevel = level;
  }
}

void updateLCDBottom(bool motorRunning, int mins, bool wifiConnected, const String &timeStr)
{
  static String prevBottom = "";
  char buf[17];
  if (motorRunning) sprintf(buf, "Motor:ON %02dM", mins);
  else sprintf(buf, "Motor:OFF     ");
  String bottom = String(buf);

  if (bottom != prevBottom) {
    lcd.setCursor(0,1);
    lcd.print(bottom);
    prevBottom = bottom;
  }

  // WiFi icon at col 10, time at col 11
  lcd.setCursor(10,1);
  if (wifiConnected) lcd.write(0);
  else {
    if (!apModeLaunched) {
      if (millis() - blinkTicker >= 500) {
        blinkTicker = millis();
        blinkState = !blinkState;
      }
      if (blinkState) lcd.write((uint8_t)0);
      else lcd.print(" ");
    } else {
      lcd.write(1);
    }
  }

  static String prevTime = "";
  if (timeStr != prevTime) {
    lcd.setCursor(11,1);
    lcd.print(timeStr);
    prevTime = timeStr;
  }
}

void setup()
{
  Serial.begin(115200);
  addLog("Booting...");

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  pinMode(switchPin, INPUT_PULLUP);

  Wire.begin(0,5);
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(1500);
  lcd.clear();

  // initial display
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(12,0); lcd.print("0%");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(1);
  lcd.setCursor(11,1); lcd.print("--:--");

  // start non-blocking WiFi connect attempt
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  connectStartMillis = millis();

  // Start Alexa now (Espalexa uses port 80 internally). Espalexa can be present even offline.
  setupAlexa();
}

// Main loop
void loop()
{
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // If not connected and AP not launched and 30s passed -> launch blocking config portal
  if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {
    addLog("30s timeout. Launching Config Portal (3 min)...");
    // Start config portal blocking call. It will block for up to 180s (timeout) or until user configures.
    bool connectedByPortal = startConfigPortalOnce();
    apModeLaunched = true; // mark AP as launched once per boot

    // After portal returns: if connectedByPortal is true -> WiFi connected
    isConnected = (WiFi.status() == WL_CONNECTED);

    // If connected, start OTA & log servers
    if (isConnected) {
      setupOTA();
      setupWebLogServer();
      addLog("WiFi Connected (via portal): " + WiFi.localIP().toString());
    } else {
      // Portal timed out or no config -> continue offline, do NOT re-enter AP this boot
      addLog("Config portal ended (no WiFi). Running offline.");
      // ensure no servers started
    }
  }

  // If connected first time via other means (not portal)
  if (isConnected && !wifiOK) {
    wifiOK = true;
    timeClient.begin();
    setupOTA();
    setupWebLogServer();
    addLog("WiFi Connected: " + WiFi.localIP().toString());
    // brief LCD notice
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(0,1); lcd.print("WiFi Connected");
    delay(1400);
    lcd.setCursor(0,1); lcd.print("Motor:OFF ");
    lcd.setCursor(10,1); lcd.write(0);
  }

  // handle web servers
  // alexaServer is used by espalexa internally (espalexa.begin(&alexaServer) called earlier)
  alexaServer.handleClient();
  if (otaStarted) otaServer.handleClient();
  if (logStarted) logServer.handleClient();
  espalexa.loop();

  // NTP time
  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
  }

  // build time string
  String currentTime = "--:--";
  if (timeSynced) {
    unsigned long elapsed = (millis() - lastSyncMillis)/1000;
    unsigned long total = offsetSeconds + elapsed;
    int h = (total/3600) % 24;
    int m = (total/60) % 60;
    char tbuf[6];
    sprintf(tbuf, "%02d:%02d", h, m);
    currentTime = String(tbuf);
  }

  // sensor readings (debounce via multiple reads)
  bool s1=false, s2=false, s3=false;
  int s4c = 0;
  for (int i=0;i<7;i++) {
    if (digitalRead(sensor1) == LOW) s1 = true;
    if (digitalRead(sensor2) == LOW) s2 = true;
    if (digitalRead(sensor3) == LOW) s3 = true;
    if (digitalRead(sensor4) == LOW) s4c++;
    delay(8);
  }
  bool s4 = (s4c >= 5);

  String level;
  if (s4 && s3 && s2 && s1) level = "100%";
  else if (s3 && s2 && s1) level = "75%";
  else if (s2 && s1) level = "50%";
  else if (s1) level = "25%";
  else level = "0%";

  globalLevel = level;

  // Update LCD top (only when changes)
  updateLCDTop(level);

  // Auto motor logic
  if (level == "0%" && !motorON) requestMotorOn("System", level);
  if (level == "100%" && motorON) requestMotorOff("System");

  // Manual switch: millis-based debounce
  int sw = digitalRead(switchPin);
  if (sw != lastSwitchState) {
    lastSwitchMillis = millis();
  }
  if ((millis() - lastSwitchMillis) > switchDebounceMs) {
    if (lastSwitchState == HIGH && sw == LOW) {
      if (motorON) requestMotorOff("Switch");
      else requestMotorOn("Switch", level);
    }
  }
  lastSwitchState = sw;

  // Update bottom line (motor status, wifi icon, time)
  int mins = 0;
  if (motorON) mins = (millis() - motorTime) / 60000;
  updateLCDBottom(motorON, mins, wifiOK, currentTime);

  // Ensure ota/log servers are started if connected later
  if (isConnected && !otaStarted) setupOTA();
  if (isConnected && !logStarted) setupWebLogServer();

  delay(160);
}
