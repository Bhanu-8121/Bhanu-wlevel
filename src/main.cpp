#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <Espalexa.h>

// =====================================================
// ALEXA SERVER → MUST USE PORT 80
// OTA → 81
// LOG VIEWER → 82
// =====================================================

Espalexa espalexa;                 // Alexa handler
ESP8266WebServer alexaServer(80);  // FIXED: Alexa on port 80
ESP8266WebServer otaServer(81);    // OTA on 81
ESP8266WebServer logServer(82);    // LOG on 82
ESP8266HTTPUpdateServer httpUpdater;

// Log storage
String serialBuffer = "";

// Add timestamped logs
void addLog(String msg) {
    unsigned long ms = millis();
    unsigned long sec = ms / 1000;
    unsigned long min = sec / 60;
    sec = sec % 60;

    char t[10];
    sprintf(t, "%02lu:%02lu", min, sec);

    String line = "[" + String(t) + "] " + msg;
    Serial.println(line);

    serialBuffer += line + "\n";
    if (serialBuffer.length() > 8000)
        serialBuffer.remove(0, 3000);
}

// WiFi / Time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

// LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi icons
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14;
const int sensor2 = 12;
const int sensor3 = 13;
const int sensor4 = 4;
const int relayPin = 16;
const int switchPin = 2;

// States
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

// =================================================
// MOTOR SAFE CONTROLS
// =================================================
void requestMotorOn(String src, String lvl) {
    if (lvl == "100%") {
        digitalWrite(relayPin, LOW);
        motorON = false;
        addLog("BLOCKED (" + src + "): Tank Full");
        return;
    }
    motorON = true;
    digitalWrite(relayPin, HIGH);
    motorTime = millis();
    addLog("Motor ON by " + src);
}

void requestMotorOff(String src) {
    motorON = false;
    digitalWrite(relayPin, LOW);
    addLog("Motor OFF by " + src);
}

// =================================================
// ALEXA CALLBACK
// =================================================
void alexaCallback(uint8_t brightness) {
    if (brightness == 0) {
        requestMotorOff("Alexa");
    } else {
        if (globalLevel == "100%")
            addLog("Alexa ON → BLOCKED (Full)");
        else
            requestMotorOn("Alexa", globalLevel);
    }
}

// =================================================
// SETUP ALEXA
// =================================================
void setupAlexa() {
    espalexa.addDevice("Water Motor", alexaCallback);
    espalexa.begin(&alexaServer);    // FIXED: use alexa server (PORT 80)
    addLog("Alexa Ready on port 80");
}

// =================================================
// WiFiManager callback
// =================================================
void configModeCallback(WiFiManager *wm) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Enter AP Mode:");
    lcd.setCursor(0, 1); lcd.print(wm->getConfigPortalSSID());
    addLog("Config Portal SSID: " + wm->getConfigPortalSSID());
}

// =================================================
// OTA
// =================================================
void setupWebOTA() {
    httpUpdater.setup(&otaServer, "/update", "kbc", "987654321");
    otaServer.begin();
    addLog("OTA Ready on port 81");
}

// =================================================
// LOG VIEWER
// =================================================
void setupWebLogServer() {
    logServer.on("/log", HTTP_GET, []() {
        logServer.send(200, "text/plain", serialBuffer);
    });
    logServer.begin();
    addLog("Log Server Ready on port 82");
}

// =================================================
// SETUP
// =================================================
void setup() {
    Serial.begin(115200);
    addLog("Booting...");

    pinMode(sensor1, INPUT_PULLUP);
    pinMode(sensor2, INPUT_PULLUP);
    pinMode(sensor3, INPUT_PULLUP);
    pinMode(sensor4, INPUT_PULLUP);
    pinMode(relayPin, OUTPUT);
    pinMode(switchPin, INPUT_PULLUP);
    digitalWrite(relayPin, LOW);

    Wire.begin(0, 5);
    lcd.init(); lcd.backlight();
    lcd.createChar(0, wifiOn);
    lcd.createChar(1, wifiOff);

    lcd.setCursor(0,0); lcd.print("Water Level:");
    lcd.setCursor(0,1); lcd.print("Waiting WiFi");

    WiFi.setAutoReconnect(true);
    WiFi.begin();

    connectStartMillis = millis();

    setupAlexa();
}

// =================================================
// LOOP
// =================================================
void loop() {

    // CONNECTION STATE
    bool isConnected = (WiFi.status() == WL_CONNECTED);

    // ---------------------------------------------------
    // WIFI MANAGER AUTOSTART
    // ---------------------------------------------------
    if (!isConnected && !apModeLaunched &&
        millis() - connectStartMillis > 30000) {

        addLog("Starting WiFiManager...");

        WiFiManager wm;
        wm.setAPCallback(configModeCallback);
        wm.setConfigPortalTimeout(180);

        if (wm.startConfigPortal("KBC-Setup", "12345678"))
            addLog("WiFi Configured");
        else
            addLog("Config Timeout → Offline Mode");

        apModeLaunched = true;
        isConnected = (WiFi.status() == WL_CONNECTED);
    }

    // ---------------------------------------------------
    // SERVER HANDLING
    // ---------------------------------------------------
    alexaServer.handleClient();   // Alexa must run ALWAYS
    if (isConnected) otaServer.handleClient();
    logServer.handleClient();
    espalexa.loop();

    // ---------------------------------------------------
    // WIFI CONNECTED ONCE
    // ---------------------------------------------------
    if (isConnected && !wifiOK) {
        wifiOK = true;

        timeClient.begin();
        setupWebOTA();
        setupWebLogServer();

        lcd.setCursor(0,1);
        lcd.print("WiFi OK         ");

        addLog("WiFi Connected: " + WiFi.localIP().toString());
    }

    // ---------------------------------------------------
    // WIFI DISCONNECTED
    // ---------------------------------------------------
    if (!isConnected && wifiOK) {
        wifiOK = false;
        addLog("WiFi Lost");
        lcd.setCursor(10,1); lcd.write(1);
    }

    // ---------------------------------------------------
    // TIME SYNC
    // ---------------------------------------------------
    if (isConnected && timeClient.update()) {
        timeSynced = true;
        lastSyncMillis = millis();
        offsetSeconds = timeClient.getHours()*3600 +
                        timeClient.getMinutes()*60 +
                        timeClient.getSeconds();
    }

    // Build Time
    String currentTime = "--:--";
    if (timeSynced) {
        unsigned long t = offsetSeconds + (millis()-lastSyncMillis)/1000;
        char buf[6];
        sprintf(buf,"%02d:%02d",(t/3600)%24,(t/60)%60);
        currentTime = buf;
    }

    // ---------------------------------------------------
    // SENSOR READING
    // ---------------------------------------------------
    bool s1=false,s2=false,s3=false;
    int s4c=0;
    for(int i=0;i<7;i++){
        if(!digitalRead(sensor1)) s1=true;
        if(!digitalRead(sensor2)) s2=true;
        if(!digitalRead(sensor3)) s3=true;
        if(!digitalRead(sensor4)) s4c++;
        delay(10);
    }
    bool s4 = (s4c>=5);

    String level;
    if(s4&&s3&&s2&&s1) level="100%";
    else if(s3&&s2&&s1) level="75%";
    else if(s2&&s1)     level="50%";
    else if(s1)         level="25%";
    else                level="0%";

    globalLevel = level;

    lcd.setCursor(12,0);
    lcd.print("    ");
    lcd.setCursor(12,0);
    lcd.print(level);

    // ---------------------------------------------------
    // AUTO MOTOR
    // ---------------------------------------------------
    if(level=="0%" && !motorON) requestMotorOn("System", level);
    if(level=="100%" && motorON) requestMotorOff("System");

    // ---------------------------------------------------
    // SWITCH
    // ---------------------------------------------------
    int sw=digitalRead(switchPin);
    if(lastSwitchState==HIGH && sw==LOW){
        if(motorON) requestMotorOff("Switch");
        else        requestMotorOn("Switch", level);
        delay(120);
    }
    lastSwitchState=sw;

    // ---------------------------------------------------
    // LCD BOTTOM
    // ---------------------------------------------------
    lcd.setCursor(0,1);
    if(motorON){
        int mins=(millis()-motorTime)/60000;
        char b[17];
        sprintf(b,"Motor:ON %02dM ",mins);
        lcd.print(b);
    } else {
        lcd.print("Motor:OFF ");
        lcd.setCursor(10,1);
        lcd.write(wifiOK ? 0 : 1);
        lcd.setCursor(11,1);
        lcd.print(currentTime);
    }

    delay(150);
}
