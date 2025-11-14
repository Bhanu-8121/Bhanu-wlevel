#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ===== WEB OTA =====
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>

// ===== ALEXA =====
#include <Espalexa.h>

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
Espalexa espalexa;

// ===== WIFI =====
// !!! UPDATE YOUR WIFI CREDENTIALS HERE !!!
const char* ssid = "KBC Hotspot";
const char* password = "fpMD@143";

WiFiUDP ntpUDP;
// Update interval 60000ms (1 min). The library handles this automatically.
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);  // IST (+05:30)

LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Symbols
byte wifiOn[8]  = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14;  // D5
const int sensor2 = 12;  // D6
const int sensor3 = 13;  // D7
const int sensor4 = 4;   // D2
const int relayPin = 16; // D0
const int manualPin = 2; // D4

// WiFi state
bool wifiOK = false;
bool firstAttempt = true;
unsigned long wifiStartTime = 0;
const unsigned long wifiTimeout = 30000;

unsigned long blinkTime = 0;
bool blinkState = false;

// --- "Single Source of Truth" for motor state ---
bool motorON = false;
bool lastMotorState = false; 
unsigned long motorTime = 0;

// --- Manual Switch Debounce ---
bool lastSwitchState = HIGH;
unsigned long lastSwitchTime = 0;
const unsigned long debounceDelay = 50;

// --- "Soft-RTC" Time ---
// This is the master switch. Once true, it stays true.
bool timeSynced = false; 
// The system time (in seconds) at the last *successful* sync
unsigned long offsetSeconds = 0;
// The millis() value at the last *successful* sync
unsigned long lastSyncMillis = 0;


// ===== WEB OTA SETUP =====
void setupWebOTA() {
  httpUpdater.setup(&server, "/update", "kbc", "987654321");  
  server.begin();
  Serial.println("WEB OTA Ready!");
  Serial.println("Go to: http://<ESP-IP>/update");
}

// ===== ALEXA CALLBACK =====
void relayChanged(uint8_t brightness) {
  Serial.print("Alexa command received: ");
  if (brightness > 0) {
    Serial.println("ON");
    motorON = true; 
  } else {
    Serial.println("OFF");
    motorON = false; 
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nBooting Water Level Controller v3...");
  Serial.println("!!! WARNING: Do not hold D4/GPIO2 switch during boot. !!!");

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(manualPin, INPUT_PULLUP);
  
  digitalWrite(relayPin, LOW); // Motor OFF by default
  lastMotorState = false;
  lastSwitchState = digitalRead(manualPin); 

  Wire.begin(0, 5); // D3 (GPIO0) and D1 (GPIO5) for I2C
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000); lcd.clear();

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(0);
  lcd.setCursor(11,1); lcd.print("--:--"); // Req 1: Initial state

  WiFi.begin(ssid, password);
  wifiStartTime = millis();
  setupWebOTA();
}

void loop() {
  // --- Core Services ---
  server.handleClient();   // Handle HTTP requests for OTA
  
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  bool ntpSuccess = false; // Flag to see if we got an NTP update *this loop*

  // --- Runs continuously if WiFi is connected ---
  if (isConnected) {
    if (!wifiOK) {
      // --- Runs ONCE on (re)connect ---
      wifiOK = true;
      firstAttempt = false;
      timeClient.begin(); // Start the NTP client
      espalexa.addDevice("Motor", relayChanged);
      espalexa.begin();
      Serial.println("WiFi Connected. Espalexa Started.");

      lcd.setCursor(0,1); lcd.print("WiFi Connected  ");
      delay(1000);
      lcd.setCursor(0,1); lcd.print("                ");
      Serial.print("IP: "); Serial.println(WiFi.localIP());
    }

    // --- Continuous WiFi tasks ---
    espalexa.loop();
    
    // timeClient.update() is smart. It only fetches new time
    // based on the 60-second interval we set.
    // It returns 'true' on a successful fetch.
    ntpSuccess = timeClient.update(); 
  }

  // --- Runs continuously if WiFi is NOT connected ---
  if (!isConnected) {
    if (wifiOK) {
      // --- Runs ONCE on disconnect ---
      wifiOK = false;
      Serial.println("WiFi Disconnected. Soft-RTC is running.");
      // !!! We DO NOT set timeSynced = false !!!
      // This is the fix for Req 3.
      
      lcd.setCursor(0,1); lcd.print("WiFi Disconnect ");
      delay(1000);
      // Display logic will take over from here
    }
    
    // Handle "WiFi Failed" message on first attempt timeout
    if (firstAttempt && (millis() - wifiStartTime >= wifiTimeout)) {
      firstAttempt = false;
      lcd.setCursor(0,1); lcd.print("WiFi Failed     ");
      delay(1000);
    }
  }


  // --- "SOFT-RTC" (Always Runs) ---
  String currentTime = "--:--";

  // Req 2 & 3: This logic runs *if* we have successfully synced *at least once*
  if (timeSynced) {
    
    // Calculate the time based on internal clock
    unsigned long elapsed = (millis() - lastSyncMillis) / 1000;
    unsigned long totalSeconds = offsetSeconds + elapsed;

    // This is the "live" part
    // If we just got a *new* update from the NTP server,
    // re-align our soft-RTC to be more accurate.
    if (ntpSuccess) {
      Serial.println("NTP re-sync! Re-aligning soft-RTC.");
      totalSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
      
      // Store the new base values
      offsetSeconds = totalSeconds;
      lastSyncMillis = millis();
    }
    
    // Format the time for display
    int h = (totalSeconds / 3600) % 24;
    int m = (totalSeconds / 60) % 60;
    char buf[6];
    sprintf(buf, "%02d:%02d", h, m);
    currentTime = String(buf);
  }
  else if (ntpSuccess) {
    // This runs only ONCE, on the very first successful sync
    Serial.println("--- INITIAL TIME SYNC ---");
    timeSynced = true; // This is the master switch
    
    // Store the base values for our soft-RTC
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
    lastSyncMillis = millis();
    
    Serial.printf("Time set to: %s\n", timeClient.getFormattedTime().c_str());
  }
  
  // --- LOGIC BLOCK 1: GATHER INPUTS ---

  // 1a. Manual Switch (non-blocking debounce)
  bool currentSwitchState = digitalRead(manualPin);
  if (currentSwitchState != lastSwitchState) {
    lastSwitchTime = millis(); // Reset debounce timer
  }

  if ((millis() - lastSwitchTime) > debounceDelay) {
    if (currentSwitchState == LOW && lastSwitchState == HIGH) {
      Serial.println("Manual switch pressed. Toggling motor.");
      motorON = !motorON; 
    }
  }
  lastSwitchState = currentSwitchState;
  
  // 1b. Water Level Sensors
  bool s1=false,s2=false,s3=false,s4=false;
  int s4c=0;
  for(int i=0;i<7;i++){
    if (digitalRead(sensor1)==LOW) s1=true;
    if (digitalRead(sensor2)==LOW) s2=true;
    if (digitalRead(sensor3)==LOW) s3=true;
    if (digitalRead(sensor4)==LOW) s4c++;
    delay(10);
  }
  s4 = (s4c>=5);

  String level;
  if (s4&&s3&&s2&&s1) level="100%";
  else if (s3&&s2&&s1) level="75%";
  else if (s2&&s1) level="50%";
  else if (s1) level="25%";
  else level="0%";

  lcd.setCursor(12,0); lcd.print("    "); 
  lcd.setCursor(12,0); lcd.print(level);


  // --- LOGIC BLOCK 2: APPLY RULES (Safety Overrides) ---
  if (level == "0%") {
    motorON = true; 
  }
  if (level == "100%") {
    motorON = false;
  }

  // --- LOGIC BLOCK 3: ACT ON FINAL STATE ---
  if (motorON && !lastMotorState) {
    motorTime = millis(); 
    Serial.println("Motor state changed to ON.");
  }
  if (!motorON && lastMotorState) {
    Serial.println("Motor state changed to OFF.");
  }
  lastMotorState = motorON; 

  digitalWrite(relayPin, motorON ? HIGH : LOW);

  // --- LCD Display (Row 1) ---
  lcd.setCursor(0,1);
  if (motorON) {
    int mins = (millis() - motorTime) / 60000;
    char buf[17]; 
    sprintf(buf, "Motor:ON frm %02dM", mins);
    lcd.print(buf);
  } else {
    // Motor is OFF
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);

    if (wifiOK) {
      lcd.write(0); // WiFi ON symbol
    }
    else if (firstAttempt){
      // Blinking symbol while connecting
      if (millis() - blinkTime > 500) {
        blinkState = !blinkState;
        blinkTime = millis();
      }
      lcd.write(blinkState ? 0 : ' ');
    }
    else {
      lcd.write(1); // WiFi OFF/Failed symbol
    }

    // This now prints either "--:--" or the live time
    lcd.setCursor(11,1); 
    lcd.print(currentTime);
  }
}

