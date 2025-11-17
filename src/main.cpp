#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h> // <-- ADDED

// ===== WEB OTA =====
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// ===== WIFI =====
// const char* ssid = "test";      // <-- REMOVED
// const char* password = "123456789"; // <-- REMOVED

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
// All 'firstAttempt' and 'wifiTimeout' variables are REMOVED

unsigned long blinkTime = 0;
bool blinkState = false;

bool motorON = false;
unsigned long motorTime = 0;

// Local RTC
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;


// ===== WiFiManager Callback =====
// This function gets called when WiFiManager enters AP mode
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
  // Username = "kbc"
  // Password = "987654321"
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

  // --- ADDED WIFI MANAGER ---
  WiFiManager wm;
  
  // Set callback for AP mode
  wm.setAPCallback(configModeCallback);

  // Set a timeout for the portal. 180 seconds (3 mins).
  // If no one configures it, it will stop the AP and continue (in offline mode).
  wm.setConfigPortalTimeout(180);

  // This is a blocking call. It will handle connecting, or
  // start the "KBC-Setup" (password: 12345678) AP.
  if (!wm.autoConnect("KBC-Setup", "12345678")) {
    Serial.println("Failed to connect and hit timeout. Running offline.");
  } else {
    Serial.println("WiFi Connected!");
  }
  
  // --- END WIFI MANAGER ---

  // Clear the AP mode message from LCD
  lcd.clear(); 

  // Enable Auto-Reconnect
  WiFi.setAutoReconnect(true); // <-- ADDED

  // ---- START WEB OTA ----
  // This MUST be after WiFiManager is done, to avoid port 80 conflict
  setupWebOTA();
}

void loop() {
  server.handleClient();   // <==== IMPORTANT FOR WEB OTA

  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // This block for 'firstAttempt' is now completely GONE.
  
  // This block runs once when WiFi connects (or reconnects)
  if (isConnected && !wifiOK) {
    wifiOK = true;
    timeClient.begin();
    if(timeClient.update()) {
      timeSynced = true;
      lastSyncMillis = millis();
      offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
    }

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
    timeSynced = false; // Time is no longer reliable
    lcd.setCursor(0,1); lcd.print("WiFi Disconnect ");
    delay(1500);
    lcd.setCursor(0,1); lcd.print("Motor:OFF       ");
    lcd.setCursor(10,1); lcd.write(1);
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

    // Simplified WiFi icon logic
    if (wifiOK) {
      lcd.write(0); // wifiOn
    } else {
      lcd.write(1); // wifiOff
    }

    lcd.setCursor(11,1); lcd.print(currentTime);
  }

  delay(200);
}
