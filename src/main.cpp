// ================================================
//   KBC — FINAL MERGED ALEXA + WIFI MANAGER CODE
// ================================================

// ---- Libraries ----
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <Espalexa.h>

// ================================================
//                GLOBAL OBJECTS
// ================================================

// Alexa MUST run on port 80
ESP8266WebServer alexaServer(80);
Espalexa espalexa;

// OTA on 81
ESP8266WebServer otaServer(81);
ESP8266HTTPUpdateServer httpUpdater;

// Log server on 82
ESP8266WebServer logServer(82);
String serialBuffer = "";

// Time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

// LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ================================================
//                CONSTANTS
// ================================================

const int sensor1 = 14; //D5
const int sensor2 = 12; //D6
const int sensor3 = 13; //D7
const int sensor4 = 4;  //D2
const int relayPin = 16; //D0
const int switchPin = 2; //D4

// WiFi Icons
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// ================================================
//                STATES
// ================================================

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

// ================================================
//               LOGGING
// ================================================

void addLog(String msg) {
    Serial.println(msg);
    serialBuffer += msg + "\n";
    if (serialBuffer.length() > 8000)
        serialBuffer.remove(0, 3000);
}

// ================================================
//         SAFE MOTOR CONTROL FUNCTIONS
// ================================================

void requestMotorOn(String src, String level)
{
    if (level == "100%") {
        motorON = false;
        digitalWrite(relayPin, LOW);
        addLog("BLOCKED: Tank Full — ON rejected (" + src + ")");
        return;
    }

    motorON = true;
    digitalWrite(relayPin, HIGH);
    motorTime = millis();
    addLog("Motor ON by " + src);
}

void requestMotorOff(String src)
{
    motorON = false;
    digitalWrite(relayPin, LOW);
    addLog("Motor OFF by " + src);
}

// ================================================
//                  ALEXA CALLBACK
// ================================================

void alexaCallback(uint8_t brightness)
{
    if (brightness == 0)
        requestMotorOff("Alexa");
    else
        requestMotorOn("Alexa", globalLevel);
}

// ================================================
//                INITIALIZATIONS
// ================================================

void setupAlexa()
{
    espalexa.addDevice("Water Motor", alexaCallback);
    espalexa.begin(&alexaServer);
    addLog("Alexa ready on port 80");
}

void configModeCallback(WiFiManager *wm)
{
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Enter AP Mode");
    lcd.setCursor(0,1); lcd.print("SSID:");
    lcd.setCursor(5,1); lcd.print(wm->getConfigPortalSSID());
    addLog("Config Portal Started");
}

void setupOTA()
{
    httpUpdater.setup(&otaServer, "/update", "kbc", "987654321");
    otaServer.begin();
    addLog("OTA Ready: http://" + WiFi.localIP().toString() + ":81/update");
}

void setupLogServer()
{
    logServer.on("/log", HTTP_GET, [](){
        logServer.send(200, "text/plain", serialBuffer);
    });
    logServer.begin();
    addLog("Log Server Ready: http://" + WiFi.localIP().toString() + ":82/log");
}

// ================================================
//                       SETUP
// ================================================

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

    Wire.begin(0,5);
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

    setupAlexa();
    addLog("Booting Complete");
}

// ================================================
//                       LOOP
// ================================================

void loop()
{
    bool isConnected = WiFi.status() == WL_CONNECTED;

    // ---- WiFi Manager 30-sec AP Mode ----
    if (!isConnected && !apModeLaunched && millis() - connectStartMillis > 30000)
    {
        WiFiManager wm;
        wm.setAPCallback(configModeCallback);
        wm.setConfigPortalTimeout(180);
        wm.startConfigPortal("KBC-Setup", "12345678");

        apModeLaunched = true;
        isConnected = WiFi.status() == WL_CONNECTED;
    }

    // ---- When WiFi gets connected first time ----
    if (isConnected && !wifiOK)
    {
        wifiOK = true;
        addLog("WiFi Connected: " + WiFi.localIP().toString());

        timeClient.begin();
        setupOTA();
        setupLogServer();

        lcd.clear();
        lcd.setCursor(0,0); lcd.print("Water Level:");
        lcd.setCursor(0,1); lcd.print("WiFi Connected");
        delay(1500);
    }

    // ---- Handle servers ----
    alexaServer.handleClient();
    otaServer.handleClient();
    logServer.handleClient();
    espalexa.loop();

    // ---- Time Sync ----
    if (isConnected && timeClient.update())
    {
        timeSynced = true;
        lastSyncMillis = millis();
        offsetSeconds =
            timeClient.getHours()*3600 +
            timeClient.getMinutes()*60 +
            timeClient.getSeconds();
    }

    String timeStr = "--:--";
    if (timeSynced)
    {
        unsigned long elapsed = (millis() - lastSyncMillis)/1000;
        unsigned long total = offsetSeconds + elapsed;

        int h = (total/3600) % 24;
        int m = (total/60) % 60;

        char buf[6];
        sprintf(buf,"%02d:%02d",h,m);
        timeStr = buf;
    }

    // ---- Read Sensors (Debounced) ----
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
    else if(s2&&s1) level="50%";
    else if(s1) level="25%";
    else level="0%";

    globalLevel = level;

    lcd.setCursor(12,0);
    lcd.print(level + " ");

    // ---- Auto Motor Logic ----
    if(level=="0%" && !motorON) requestMotorOn("System", level);
    if(level=="100%" && motorON) requestMotorOff("System");

    // ---- Switch Control ----
    int sw = digitalRead(switchPin);
    if(lastSwitchState == HIGH && sw == LOW)
    {
        if (motorON) requestMotorOff("Switch");
        else requestMotorOn("Switch", level);
        delay(80);
    }
    lastSwitchState = sw;

    // ---- LCD Bottom Line ----
    lcd.setCursor(0,1);
    if(motorON)
    {
        int mins = (millis() - motorTime)/60000;
        char buf[16];
        sprintf(buf,"Motor:ON %02dM",mins);
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
