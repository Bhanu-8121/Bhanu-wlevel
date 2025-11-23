/* Water motor Alexa - FINAL + WEB SERIAL MONITOR (PORT 82)
   - Alexa on port 80
   - OTA on port 81
   - Serial monitor webpage on port 82 → http://<IP>/log
   - Manual switch on D4 (GPIO2)
   - 100% tank lock
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

// ==== Alexa ====
Espalexa espalexa;

// ==== OTA on port 81 ====
ESP8266WebServer server(81);
ESP8266HTTPUpdateServer httpUpdater;

// ==== WEB SERIAL MONITOR on port 82 ====
ESP8266WebServer logServer(82);
String serialBuffer = "";

// Function to record logs
void addLog(String msg) {
    Serial.println(msg);

    serialBuffer += msg + "\n";

    // Limit size (keep last ~200 lines)
    if (serialBuffer.length() > 8000)
        serialBuffer.remove(0, 3000);
}

// ==== WIFI / TIME ====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Icons
byte wifiOn[8] = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14;
const int sensor2 = 12;
const int sensor3 = 13;
const int sensor4 = 4;
const int relayPin = 16;
const int switchPin = 2;

// State
bool wifiOK = false;
bool apModeLaunched = false;
unsigned long connectStartMillis = 0;

bool motorON = false;
unsigned long motorTime = 0;

bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

String globalLevel = "0%";
int lastSwitchState = HIGH;


// =========================================
//        MOTOR CONTROL SAFE LOGIC
// =========================================

void requestMotorOn(String source, String level)
{
    if (level == "100%")
    {
        motorON = false;
        digitalWrite(relayPin, LOW);
        espalexa.getDevice(0)->setValue(0);

        addLog("BLOCKED: Tank full → ON rejected (" + source + ")");
        return;
    }

    motorON = true;
    digitalWrite(relayPin, HIGH);
    motorTime = millis();

    espalexa.getDevice(0)->setValue(255);

    addLog("Motor ON by " + source);
}

void requestMotorOff(String source)
{
    motorON = false;
    digitalWrite(relayPin, LOW);
    espalexa.getDevice(0)->setValue(0);
    addLog("Motor OFF by " + source);
}


// =========================================
//              ALEXA CALLBACK
// =========================================

void alexaCallback(uint8_t id, bool state)
{
    String level = globalLevel;

    if (state) {
        if (level == "100%") {
            motorON = false;
            digitalWrite(relayPin, LOW);
            espalexa.getDevice(0)->setValue(0);
            addLog("Alexa tried ON → BLOCKED (full tank)");
        }
        else requestMotorOn("Alexa", level);
    }
    else requestMotorOff("Alexa");
}


// =========================================
//              ALEXA SETUP
// =========================================
void setupAlexa()
{
    espalexa.addDevice("Water Motor", alexaCallback);
    espalexa.begin();
    addLog("Alexa device added: Water Motor");
}


// =========================================
//           WIFI MANAGER CALLBACK
// =========================================

void configModeCallback(WiFiManager *wm)
{
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Enter AP Mode");
    lcd.setCursor(0, 1); lcd.print("SSID:");
    lcd.setCursor(5, 1); lcd.print(wm->getConfigPortalSSID());

    addLog("Config Portal Started: " + wm->getConfigPortalSSID());
}


// =========================================
//              OTA SETUP
// =========================================

void setupWebOTA()
{
    httpUpdater.setup(&server, "/update", "kbc", "987654321");
    server.begin();

    addLog("OTA Ready: http://" + WiFi.localIP().toString() + ":81/update");
}


// =========================================
//          WEB SERIAL MONITOR (82)
// =========================================

void setupWebLogServer()
{
    logServer.on("/log", HTTP_GET, []() {
        logServer.send(200, "text/plain", serialBuffer);
    });

    logServer.begin();
    addLog("Web Serial Log ready: http://" + WiFi.localIP().toString() + ":82/log");
}


// =========================================
//                 SETUP
// =========================================

void setup()
{
    Serial.begin(115200);

    pinMode(sensor1, INPUT_PULLUP);
    pinMode(sensor2, INPUT_PULLUP);
    pinMode(sensor3, INPUT_PULLUP);
    pinMode(sensor4, INPUT_PULLUP);
    pinMode(relayPin, OUTPUT);
    digitalWrite(relayPin, LOW);

    pinMode(switchPin, INPUT_PULLUP);

    // LCD
    Wire.begin(0, 5);
    lcd.init(); lcd.backlight();
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

    WiFi.setAutoReconnect(true);
    WiFi.begin();
    connectStartMillis = millis();

    addLog("Booting...");
    setupAlexa();
}


// =========================================
//                 LOOP
// =========================================

void loop()
{
    bool isConnected = (WiFi.status() == WL_CONNECTED);

    if (!isConnected && !apModeLaunched && millis() - connectStartMillis > 30000)
    {
        WiFiManager wm;
        wm.setAPCallback(configModeCallback);
        wm.setConfigPortalTimeout(180);
        wm.startConfigPortal("KBC-Setup", "12345678");
        apModeLaunched = true;
        isConnected = (WiFi.status() == WL_CONNECTED);
    }

    if (isConnected && !wifiOK)
    {
        wifiOK = true;
        timeClient.begin();
        setupWebOTA();
        setupWebLogServer();  // <-- NEW

        addLog("WiFi Connected: " + WiFi.localIP().toString());

        lcd.clear();
        lcd.setCursor(0,0); lcd.print("Water Level:");
        lcd.setCursor(0,1); lcd.print("WiFi Connected");
        delay(1500);
    }

    if (isConnected) server.handleClient();
    logServer.handleClient();   // <-- NEW

    espalexa.loop();

    // Time
    if (isConnected && timeClient.update())
    {
        timeSynced = true;
        lastSyncMillis = millis();
        offsetSeconds =
            timeClient.getSeconds() +
            timeClient.getMinutes()*60 +
            timeClient.getHours()*3600;
    }

    String timeStr = "--:--";
    if (timeSynced)
    {
        unsigned long elapsed = (millis() - lastSyncMillis) / 1000;
        unsigned long total = offsetSeconds + elapsed;

        int h = (total / 3600) % 24;
        int m = (total / 60) % 60;

        char buf[6];
        sprintf(buf, "%02d:%02d", h, m);
        timeStr = buf;
    }

    // READ SENSORS
    bool s1=false,s2=false,s3=false;
    int s4c=0;
    for (int i=0; i<7; i++) {
        if (digitalRead(sensor1)==LOW) s1=true;
        if (digitalRead(sensor2)==LOW) s2=true;
        if (digitalRead(sensor3)==LOW) s3=true;
        if (digitalRead(sensor4)==LOW) s4c++;
        delay(10);
    }
    bool s4 = (s4c >= 5);

    String level;
    if (s4&&s3&&s2&&s1) level="100%";
    else if (s3&&s2&&s1) level="75%";
    else if (s2&&s1) level="50%";
    else if (s1) level="25%";
    else level="0%";

    globalLevel = level;

    lcd.setCursor(0,0);
    lcd.print("Water Level:");
    lcd.setCursor(12,0);
    lcd.print(level + " ");

    // AUTO MOTOR LOGIC
    if (level=="0%" && !motorON) requestMotorOn("System", level);
    if (level=="100%" && motorON) requestMotorOff("System");

    // SWITCH
    int sw = digitalRead(switchPin);
    if (lastSwitchState == HIGH && sw == LOW)
    {
        if (motorON) requestMotorOff("Switch");
        else requestMotorOn("Switch", level);
        delay(80);
    }
    lastSwitchState = sw;

    // LCD BOTTOM
    lcd.setCursor(0,1);
    if (motorON)
    {
        int mins = (millis() - motorTime)/60000;
        char buf[16];
        sprintf(buf, "Motor:ON %02dM", mins);
        lcd.print(buf);
    }
    else
    {
        lcd.print("Motor:OFF ");
        lcd.setCursor(10,1);
        lcd.write(wifiOK ? 0 : 1);
    }

    lcd.setCursor(11,1);
    lcd.print(timeStr);

    delay(200);
}

