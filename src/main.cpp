/***************************
    YOUR ORIGINAL INCLUDES
****************************/
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h> 

/***************************
    >>> ALEXA SUPPORT <<<
****************************/
#include <fauxmoESP.h>
fauxmoESP fauxmo;

/***************************
    ORIGINAL OBJECTS
****************************/
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

LiquidCrystal_I2C lcd(0x27, 16, 2);

/***************************
     PIN DEFINITIONS
****************************/
const int sensor1 = 14; // D5
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 4;  // D2
const int relayPin = 16; // D0

// <<< ADDED: Manual Switch >>>
const int switchPin = 2; // D4 (GPIO2)

/***************************
    EXISTING VARIABLES
****************************/
bool wifiOK = false;
unsigned long connectStartMillis = 0;
bool apModeLaunched = false;
bool motorON = false;
unsigned long motorTime = 0;

/***************************
    RTC variables
****************************/
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

/***************************
    BLINKER
****************************/
unsigned long blinkTicker = 0;
bool blinkState = false;

/***************************
  CUSTOM FUNCTIONS ADDED
****************************/

// SAFE MOTOR ON (Alexa + Switch + Auto)
void requestMotorOn(String source, String level) {

    if (level == "100%") {
        // Tank full — BLOCK turning on
        motorON = false;
        digitalWrite(relayPin, LOW);

        Serial.println("Tank full → Motor blocked");

        // ALEXA voice response
        fauxmo.setState("water motor", false, 0);
        fauxmo.reportState("water motor");

        return;
    }

    // Tank NOT full → allow ON
    motorON = true;
    digitalWrite(relayPin, HIGH);
    motorTime = millis();

    Serial.println("Motor turned ON by " + source);

    // Alexa sync ON
    fauxmo.setState("water motor", true, 255);
    fauxmo.reportState("water motor");
}

// SAFE MOTOR OFF
void requestMotorOff(String source) {

    motorON = false;
    digitalWrite(relayPin, LOW);

    Serial.println("Motor turned OFF by " + source);

    // Alexa sync OFF
    fauxmo.setState("water motor", false, 0);
    fauxmo.reportState("water motor");
}

/***************************
  >>> ALEXA SETUP <<<
****************************/
void setupAlexa() {
    fauxmo.createServer(false);
    fauxmo.setPort(80);
    fauxmo.enable(true);

    fauxmo.addDevice("water motor");

    fauxmo.onSetState([](unsigned char device_id, const char *device_name, bool state, unsigned char value) {

        String level = "0%"; // default (will be updated by loop)
        extern String globalLevel;
        level = globalLevel;

        if (state) {
            requestMotorOn("Alexa", level);
        } else {
            requestMotorOff("Alexa");
        }
    });

    Serial.println("Alexa Ready.");
}

/***************************
     GLOBAL LEVEL VARIABLE
****************************/
String globalLevel = "0%";

/***************************
          SETUP
****************************/
void setup() {
    Serial.begin(115200);

    pinMode(sensor1, INPUT_PULLUP);
    pinMode(sensor2, INPUT_PULLUP);
    pinMode(sensor3, INPUT_PULLUP);
    pinMode(sensor4, INPUT_PULLUP);
    pinMode(relayPin, OUTPUT);
    digitalWrite(relayPin, LOW);

    // <<< ADDED SWITCH >>>
    pinMode(switchPin, INPUT_PULLUP);

    Wire.begin(0, 5);
    lcd.init(); lcd.backlight();

    // WiFi Connect
    WiFi.setAutoReconnect(true);
    WiFi.begin();
    connectStartMillis = millis();

    // <<< ADDED FOR ALEXA >>>
    setupAlexa();
}

/***************************
            LOOP
****************************/
void loop() {

    /***********************
     ORIGINAL WIFI LOGIC
    ************************/
    bool isConnected = (WiFi.status() == WL_CONNECTED);

    if (isConnected) {
        server.handleClient();
        fauxmo.handle();   // <<< HANDLE ALEXA >>>
    }

    /***********************
     SENSOR READING
    ************************/
    bool s1=false,s2=false,s3=false,s4=false;
    int s4c=0;

    for(int i=0;i<7;i++){
        if (!digitalRead(sensor1)) s1=true;
        if (!digitalRead(sensor2)) s2=true;
        if (!digitalRead(sensor3)) s3=true;
        if (!digitalRead(sensor4)) s4c++;
        delay(10);
    }
    s4 = (s4c>=5);

    // LEVEL CALCULATION
    String level;
    if (s4&&s3&&s2&&s1) level="100%";
    else if (s3&&s2&&s1) level="75%";
    else if (s2&&s1) level="50%";
    else if (s1) level="25%";
    else level="0%";

    globalLevel = level;

    /***********************
      AUTO MOTOR LOGIC
    ************************/
    if (level=="0%" && !motorON){
        requestMotorOn("System", level);
    }

    if (level=="100%" && motorON){
        requestMotorOff("System");
    }

    /***********************
      >>> SWITCH CONTROL <<<
    ************************/
    static bool lastSwitch = HIGH;
    bool currentSwitch = digitalRead(switchPin);

    if (currentSwitch == LOW && lastSwitch == HIGH) {
        // TOGGLE / PRESS EVENT
        if (motorON) {
            requestMotorOff("Switch");
        } else {
            requestMotorOn("Switch", level);
        }
    }
    lastSwitch = currentSwitch;

    /***********************
      ORIGINAL LCD LOGIC
    ************************/

    lcd.setCursor(0,0);
    lcd.print("Water Level:");
    lcd.setCursor(12,0);
    lcd.print(level);

    lcd.setCursor(0,1);
    if (motorON) {
        int mins=(millis()-motorTime)/60000;
        char buf[17];
        sprintf(buf,"Motor:ON %02dM",mins);
        lcd.print(buf);
    } else {
        lcd.print("Motor:OFF ");
    }

    delay(200);
}
