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

// Function to record logs to serial and web buffer
void addLog(String msg) {
    Serial.println(msg);
    serialBuffer += msg + "\n";
    // Keep buffer size manageable
    if (serialBuffer.length() > 8000)
        serialBuffer.remove(0, 3000);
}

// ==== WIFI / TIME ====
WiFiUDP ntpUDP;
// Time zone offset set for IST (5.5 hours * 3600 seconds = 19800)
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Icons
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins (Assuming standard NodeMCU D-pin mapping)
const int sensor1 = 14; // D5 (Bottom)
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 4;  // D2 (Top)
const int relayPin = 16; // D0 (Relay control)
const int switchPin = 2; // D4 (Manual switch)

// WiFi state
bool wifiOK = false;

// WiFi Manager State
unsigned long connectStartMillis = 0;
bool apModeLaunched = false; // Flag to ensure the Config Portal logic runs only once

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
int lastSwitchState = HIGH; // Tracks manual switch for falling edge detection

// =========================================
//        MOTOR CONTROL SAFE LOGIC
// =========================================
void requestMotorOn(String source, String level)
{
    if (level == "100%") {
        // Safety check: Do not turn on if the tank is full
        motorON = false;
        digitalWrite(relayPin, LOW);
        addLog("BLOCKED: Tank full -> ON rejected (" + source + ")");
        return;
    }
    if (!motorON) {
        motorON = true;
        digitalWrite(relayPin, HIGH);
        motorTime = millis();
        addLog("Motor ON by " + source + " (Level: " + level + ")");
    } else {
        addLog("Motor already ON. ON request from " + source + " ignored.");
    }
}

void requestMotorOff(String source)
{
    if (motorON) {
        motorON = false;
        digitalWrite(relayPin, LOW);
        addLog("Motor OFF by " + source);
    } else {
        addLog("Motor already OFF. OFF request from " + source + " ignored.");
    }
}

// =========================================
//              ALEXA CALLBACK
// =========================================
void alexaCallback(uint8_t brightness)
{
    String level = globalLevel;
    if (brightness == 0) {
        requestMotorOff("Alexa");
    } else {
        // Alexa ON request checks the current tank level
        requestMotorOn("Alexa", level);
    }
    // Update the Espalexa state to match the motor (e.g., if blocked)
    espalexa.getDevice(0).setValue(motorON ? 255 : 0);
}

void setupAlexa()
{
    // The device name "Water Motor" will appear in the Alexa app
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
        // Simple HTML for web log viewing
        String html = "<!DOCTYPE html><html><head><title>KBC Log</title><meta http-equiv=\"refresh\" content=\"5\"></head><body>";
        html += "<pre>" + serialBuffer + "</pre>";
        html += "</body></html>";
        logServer.send(200, "text/html", html);
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

    // Pin setup
    pinMode(sensor1, INPUT_PULLUP);
    pinMode(sensor2, INPUT_PULLUP);
    pinMode(sensor3, INPUT_PULLUP);
    pinMode(sensor4, INPUT_PULLUP);
    pinMode(relayPin, OUTPUT);
    digitalWrite(relayPin, LOW); // Ensure motor is off on boot
    pinMode(switchPin, INPUT_PULLUP);

    // LCD Setup
    Wire.begin(0, 5); // Assuming I2C pins D2/SDA (GPIO4) and D1/SCL (GPIO5) on NodeMCU
    lcd.init(); lcd.backlight();
    lcd.createChar(0, wifiOn);
    lcd.createChar(1, wifiOff);

    // Boot display
    lcd.setCursor(6,0); lcd.print("K.B.C");
    lcd.setCursor(0,1); lcd.print("Home Automation");
    delay(2000);
    lcd.clear();

    // Default offline display
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(0,1); lcd.print("Motor:OFF ");
    lcd.setCursor(10,1); lcd.write(1); // WiFi Off icon
    lcd.setCursor(11,1); lcd.print("--:--");

    // Initial WiFi connection attempt
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

    // --- Core WiFiManager Logic from Sample Code ---
    // If not connected AND we are still within the initial 30s attempt AND haven't launched AP yet
    if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {
        addLog("30s timeout. Launching Config Portal...");
        WiFiManager wm;
        wm.setAPCallback(configModeCallback);
        wm.setConfigPortalTimeout(180); // 3 minutes timeout
        if (wm.startConfigPortal("KBC-Setup", "12345678")) {
            addLog("AP connection successful!");
        } else {
            addLog("AP timeout. Running offline.");
        }
        apModeLaunched = true; // Set flag so the blocking AP never runs again
        // Re-evaluate connection after AP attempt
        isConnected = (WiFi.status() == WL_CONNECTED);
    }
    // --- End WiFiManager Logic ---

    // Handle network clients
    if (isConnected) {
        server.handleClient();   // OTA (Port 81)
    }
    logServer.handleClient();    // Web Log (Port 82)
    espalexa.loop();             // Alexa discovery and control

    // Runs once when WiFi connects (or reconnects)
    if (isConnected && !wifiOK) {
        wifiOK = true;
        timeClient.begin();
        setupWebOTA();
        setupWebLogServer();

        // ** CRUCIAL: Set apModeLaunched = true here **
        // This prevents the 30-second AP mode check from ever triggering again
        // once a stable connection has been achieved.
        apModeLaunched = true;

        lcd.clear();
        lcd.setCursor(0,0); lcd.print("Water Level:");
        lcd.setCursor(0,1); lcd.print("WiFi Connected ");
        delay(1500);
        lcd.setCursor(0,1); lcd.print("Motor:OFF ");
        lcd.setCursor(10,1); lcd.write(0); // Stable WiFi On icon

        addLog("WiFi Connected: " + WiFi.localIP().toString());
    }

    // Runs once when WiFi disconnects
    if (!isConnected && wifiOK) {
        wifiOK = false;
        lcd.setCursor(0,1); lcd.print("WiFi Disconnect ");
        delay(1500);
        lcd.setCursor(0,1); lcd.print("Motor:OFF ");
        lcd.setCursor(10,1); lcd.write(1); // Stable WiFi Off icon
        addLog("WiFi Disconnected");
    }

    // --- Maintain NTP Time Sync (soft RTC) ---
    if (isConnected && timeClient.update()) {
        timeSynced = true;
        lastSyncMillis = millis();
        // Recalculate offset based on NTP time
        offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
    }

    // Build current time string
    String currentTime = "--:--";
    if (timeSynced) {
        // Calculate elapsed time since last NTP sync and add to offset
        unsigned long elapsed = (millis() - lastSyncMillis)/1000;
        unsigned long total = offsetSeconds + elapsed;
        int h = (total/3600) % 24; // Hours (0-23)
        int m = (total/60) % 60;   // Minutes (0-59)
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
    bool s4 = (s4c>=5); // S4 requires 5 out of 7 readings to be LOW

    // Level logic
    String level;
    if (s4&&s3&&s2&&s1) level="100%";
    else if (s3&&s2&&s1) level="75%";
    else if (s2&&s1)     level="50%";
    else if (s1)         level="25%";
    else                 level="0%";

    globalLevel = level; // Update for use by Alexa and other modules

    // Update LCD - Level (top line)
    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(12,0); lcd.print("    "); // clear area
    lcd.setCursor(12,0); lcd.print(level);

    // --- Auto Motor Logic ---
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
        delay(80); // Debounce delay
    }
    lastSwitchState = sw;

    // --- Update LCD Bottom Line: motor status, wifi icon, time ---
    lcd.setCursor(0,1);
    if (motorON)
    {
        int mins = (millis() - motorTime)/60000;
        char buf[17];
        sprintf(buf, "Motor:ON %02dM  ", mins);
        lcd.print(buf);
    }
    else
    {
        lcd.print("Motor:OFF ");
        lcd.setCursor(10,1);

        // WiFi icon logic (Blinking during initial 30s attempt, or stable)
        if (wifiOK) {
            lcd.write(0); // Stable WiFi On icon
        } else {
            // If not connected, check if still in initial 30s window
            if (!apModeLaunched) {
                // Blink the 'On' icon to show active connection attempt
                if (millis() - blinkTicker >= 500) {
                    blinkTicker = millis();
                    blinkState = !blinkState;
                }
                if (blinkState) lcd.write((uint8_t)0); // WiFi On
                else lcd.print(" ");                   // Space
            } else {
                // AP already attempted (offline mode)
                lcd.write(1); // Stable WiFi Off icon
            }
        }
    }

    // Time on the right
    lcd.setCursor(11,1);
    lcd.print(currentTime);

    delay(200);
}
