V6 

//Alexa working
# include <Wire.h>
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

    // Build timestamp using soft RTC
    String ts = "--:--";
    if (timeSynced) {
        unsigned long elapsed = (millis() - lastSyncMillis)/1000;
        unsigned long total = offsetSeconds + elapsed;
        int h = (total/3600) % 24;
        int m = (total/60) % 60;

        char buf[6];
        sprintf(buf, "%02d:%02d", h, m);
        ts = String(buf);
    }

    // Final log line
    String line = "[" + ts + "] " + msg;

    Serial.println(line);

    serialBuffer += line + "\n";

    if (serialBuffer.length() > 8000)
        serialBuffer.remove(0, 3000);
}

// ==== WIFI / TIME ====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Icons
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14;
const int sensor2 = 12;
const int sensor3 = 13;
const int sensor4 = 4;
const int relayPin = 16;
const int switchPin = 2;

// WiFi state
bool wifiOK = false;

// WiFi Manager State
unsigned long connectStartMillis = 0;
bool apModeLaunched = false; // AP only runs once

// Motor state
bool motorON = false;
unsigned long motorTime = 0;

// Local RTC
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

// Blink variables
unsigned long blinkTicker = 0;
bool blinkState = false;

String globalLevel = "0%";
int lastSwitchState = HIGH;

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
    espalexa.begin();
    addLog("Alexa device added: Water Motor");
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

// =========================================
//                  SETUP
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
    Wire.begin(0, 5); // Adjust if your I2C pins differ (e.g., D2=4 SDA, D1=5 SCL)
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
//                   LOOP
// =========================================
void loop()
{
    // Connection state
    bool isConnected = (WiFi.status() == WL_CONNECTED);

    // --- WiFi/AP Manager Logic ---
    if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {
        addLog("30s timeout. Launching Config Portal...");
        WiFiManager wm;
        wm.setAPCallback(configModeCallback);
        wm.setConfigPortalTimeout(180); // 3 minutes
        if (wm.startConfigPortal("KBC-Setup", "12345678")) {
            addLog("AP connection successful!");
        } else {
            addLog("AP timeout. Running offline.");
        }
        apModeLaunched = true; // run only once
        // Re-evaluate connection after AP
        isConnected = (WiFi.status() == WL_CONNECTED);
    }
    // --- End WiFi/AP Logic ---

    // Handle servers every loop
    if (isConnected) {
        server.handleClient();   // OTA
    }
    logServer.handleClient();    // Web Log
    espalexa.loop();             // Alexa

    // Runs once when WiFi connects (or reconnects)
    if (isConnected && !wifiOK) {
        wifiOK = true;
        timeClient.begin();        // Start NTP
        setupWebOTA();             // Start OTA server
        setupWebLogServer();       // Start log server

        // After a successful connection, disable future AP attempts
        apModeLaunched = true;

   /*     lcd.clear();
        lcd.setCursor(0,0); lcd.print("Water Level:");
        lcd.setCursor(0,1); lcd.print("WiFi Connected ");
        delay(1500);
        lcd.setCursor(0,1); lcd.print("Motor:OFF ");
        lcd.setCursor(10,1); lcd.write(0);

        addLog("WiFi Connected: " + WiFi.localIP().toString());*/
     // --showing ip address here -------
    lcd.clear();
    lcd.setCursor(0,0); 
    lcd.print("WiFi Connected");
    lcd.setCursor(0,1);
    lcd.print(WiFi.localIP());     // <--- NEW
    delay(2000);

    // ------- Return to normal screen -------
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
        unsigned long elapsed = (millis() - lastSyncMillis)/1000;
        unsigned long total = offsetSeconds + elapsed;
        int h = (total/3600) % 24;
        int m = (total/60) % 60;
        char buf[6];
        sprintf(buf, "%02d:%02d", h, m);
        currentTime = String(buf);
    }

    // --- Sensor reading with de-bounce ---
    bool s1=false,s2=false,s3=false;
    int s4c=0;
    for(int i=0;i<7;i++){
        if (digitalRead(sensor1)==LOW) s1=true;
        if (digitalRead(sensor2)==LOW) s2=true;
        if (digitalRead(sensor3)==LOW) s3=true;
        if (digitalRead(sensor4)==LOW) s4c++;
        delay(10);
    }
    bool s4 = (s4c>=5);

    // Level logic
    String level;
    if (s4&&s3&&s2&&s1) level="100%";
    else if (s3&&s2&&s1) level="75%";
    else if (s2&&s1)     level="50%";
    else if (s1)         level="25%";
    else                 level="0%";

    globalLevel = level; // keep Alexa logic in sync

    // Update LCD - Level (top line)
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(12,0); lcd.print("    "); // clear area
    lcd.setCursor(12,0); lcd.print(level);

    // --- Auto Motor Logic with safety wrappers ---
    if (level=="0%" && !motorON) {
        requestMotorOn("System", level);
    }
    if (level=="100%" && motorON) {
        requestMotorOff("System");
    }

    // --- Manual switch (toggle on falling edge) ---
    int sw = digitalRead(switchPin);
    if (lastSwitchState == HIGH && sw == LOW)
    {
        if (motorON) requestMotorOff("Switch");
        else requestMotorOn("Switch", level);
        delay(80); // debounce
    }
    lastSwitchState = sw;

    // --- Bottom line: motor status, wifi icon, time ---
    lcd.setCursor(0,1);
    if (motorON)
    {
        int mins = (millis() - motorTime)/60000;
        char buf[17]; // 16 chars + null
        sprintf(buf, "Motor:ON %02dM  ", mins);
        lcd.print(buf);
    }
    else
    {
        lcd.print("Motor:OFF ");
        lcd.setCursor(10,1);

        // WiFi icon logic
        if (wifiOK) {
            lcd.write(0); // wifiOn (Stable)
        } else {
            // If not connected, check if still in initial 30s window → blink
            if (!apModeLaunched) {
                if (millis() - blinkTicker >= 500) {
                    blinkTicker = millis();
                    blinkState = !blinkState;
                }
                if (blinkState) lcd.write((uint8_t)0); // wifiOn
                else lcd.print(" "); // Space
            } else {
                // AP already attempted → permanently offline
                lcd.write(1); // wifiOff (Stable)
            }
        }
    }

    // Time on the right
    lcd.setCursor(11,1);
    lcd.print(currentTime);

    delay(200);
}
