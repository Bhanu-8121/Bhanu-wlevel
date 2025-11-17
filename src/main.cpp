#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h> 

// ===== WEB OTA =====
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// ===== WIFI =====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);  // IST

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

// WiFi state
bool wifiOK = false;

// WiFi Manager State
unsigned long connectStartMillis = 0;
bool apModeLaunched = false; // Flag to ensure AP only runs once

bool motorON = false;
unsigned long motorTime = 0;

// Local RTC
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

// --- BLINK FIX: Added variables ---
unsigned long blinkTicker = 0;
bool blinkState = false;
// ---

// ===== WiFiManager Callback =====
void configModeCallback(WiFiManager *myWiFiManager) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Enter AP Mode");
  lcd.setCursor(0, 1); lcd.print("SSID:");
  lcd.setCursor(5, 1); lcd.print(myWiFiManager->getConfigPortalSSID());
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}


// ===== WEB OTA SETUP (iPhone-compatible) =====
void setupWebOTA() {
  httpUpdater.setup(&server, "/update", "kbc", "987654321");  
  server.begin();
  Serial.println("WEB OTA Ready!");
  Serial.print("Go to: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/update");
}


void setup() {
  Serial.begin(115200);

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  Wire.begin(0, 5);
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000); lcd.clear();

  // Print the default "offline" screen
  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(1); // Show WiFi Off icon
  lcd.setCursor(11,1); lcd.print("--:--");

  // Start non-blocking WiFi connection
  WiFi.setAutoReconnect(true); 
  WiFi.begin();
  connectStartMillis = millis();
}

void loop() {
  
  // --- WiFi/AP Manager Logic ---
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  
  // Check if 30s have passed, we're not connected, and we haven't launched the AP yet
  if (!isConnected && !apModeLaunched && (millis() - connectStartMillis > 30000)) {
    Serial.println("30s timeout. Launching Config Portal...");
    
    // Launch the blocking AP. This will pause the loop for 3 mins.
    WiFiManager wm;
    wm.setAPCallback(configModeCallback);
    wm.setConfigPortalTimeout(180); // 3 minutes
    
    if (wm.startConfigPortal("KBC-Setup", "12345678")) {
        Serial.println("AP connection successful!");
    } else {
        Serial.println("AP timeout. Running offline.");
    }
    apModeLaunched = true; // Set flag so we don't run this again
    
    // Check if we are NOW connected (after AP)
    isConnected = (WiFi.status() == WL_CONNECTED);
  }
  // --- End WiFi/AP Logic ---


  // This must be checked every loop
  if (isConnected) {
    server.handleClient(); // Handle OTA requests
  }


  // This block runs once when WiFi connects (or reconnects)
  if (isConnected && !wifiOK) {
    wifiOK = true;
    timeClient.begin(); // Start the time client
    setupWebOTA(); // Initialize OTA server

    lcd.clear(); // Clear screen to show "WiFi Connected"
    lcd.setCursor(0,0); lcd.print("Water Level:"); // Re-print top line
    lcd.setCursor(0,1); lcd.print("WiFi Connected  ");
    delay(1500);
    lcd.setCursor(0,1); lcd.print("                "); // Clear bottom line

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }

  // This block runs once when WiFi disconnects
  if (!isConnected && wifiOK) {
    wifiOK = false;
    
    lcd.setCursor(0,1); lcd.print("WiFi Disconnect ");
    delay(1500);
    lcd.setCursor(0,1); lcd.print("Motor:OFF       ");
    lcd.setCursor(10,1); lcd.write(1);
  }

  // --- Maintain NTP Time Sync ---
  if (isConnected && timeClient.update()) {
    timeSynced = true;
    lastSyncMillis = millis();
    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
  }


  // Get current time (either from "soft RTC" or "--:--")
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

  // Sensor reading (with de-bounce)
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

  // Level logic
  String level;
  if (s4&&s3&&s2&&s1) level="100%";
  else if (s3&&s2&&s1) level="75%";
  else if (s2&&s1) level="50%";
  else if (s1) level="25%";
  else level="0%";

  // Update LCD - Level
  lcd.setCursor(0,0); lcd.print("Water Level:"); // Re-print in case it was cleared
  lcd.setCursor(12,0); lcd.print("    "); 
  lcd.setCursor(12,0); lcd.print(level);

  // Motor Logic
  if (level=="0%" && !motorON){
    motorON=true; digitalWrite(relayPin,HIGH); motorTime=millis();
  }
  if (level=="100%" && motorON){
    motorON=false; digitalWrite(relayPin,LOW);
  }

  // Update LCD - Bottom Line
  lcd.setCursor(0,1);
  if (motorON){
    int mins=(millis()-motorTime)/60000;
    char buf[17]; // 16 chars + null
    sprintf(buf,"Motor:ON frm %02dM",mins);
    lcd.print(buf);
    lcd.print(" "); // Clear rest of line
  }
  else{
    lcd.print("Motor:OFF ");
    lcd.setCursor(10,1);

    // --- BLINK FIX: Updated WiFi icon logic ---
    if (wifiOK) {
      lcd.write(0); // wifiOn (Stable)
    } else {
      // If not connected, check if we are in the initial 30s window
      if (!apModeLaunched) { 
        // We are in the first 30s, so BLINK the 'On' icon
        if (millis() - blinkTicker >= 500) {
          blinkTicker = millis();
          blinkState = !blinkState;
        }
        if (blinkState) lcd.write((uint8_t)0); // wifiOn
        else lcd.print(" "); // Space
      } else {
        // AP has been tried, we are permanently offline
        lcd.write(1); // wifiOff (Stable)
      }
    }
    // --- END BLINK FIX ---

    lcd.setCursor(11,1); lcd.print(currentTime);
  }

  delay(200);
}
