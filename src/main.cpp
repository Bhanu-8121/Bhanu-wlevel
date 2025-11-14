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
const char* ssid = "test";
const char* password = "123456789";

WiFiUDP ntpUDP;
// Update interval set to 60000ms (60 seconds)
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

// --- NEW --- Manual Switch Pin
// D4 (GPIO2) is used for the onboard LED.
// !!! WARNING: This pin MUST be HIGH at boot. !!!
// Do NOT hold the manual switch down when powering on the ESP.
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
bool lastMotorState = false; // For detecting changes
unsigned long motorTime = 0;

// --- NEW --- Manual Switch Debounce
bool lastSwitchState = HIGH;
unsigned long lastSwitchTime = 0;
const unsigned long debounceDelay = 50;

// Local RTC
bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;
unsigned long lastNtpResync = 0;

// ===== WEB OTA SETUP =====
void setupWebOTA() {
  // Username = "kbc"
  // Password = "987654321"
  httpUpdater.setup(&server, "/update", "kbc", "987654321");  
  server.begin();
  Serial.println("WEB OTA Ready!");
  Serial.println("Go to: http://<ESP-IP>/update");
}

// ===== NEW --- ALEXA CALLBACK =====
void relayChanged(uint8_t brightness) {
  Serial.print("Alexa command received: ");
  if (brightness > 0) {
    Serial.println("ON");
    motorON = true; // Set the master variable
  } else {
    Serial.println("OFF");
    motorON = false; // Set the master variable
  }
}


void setup() {
  Serial.begin(115200);
  Serial.println("\n\nBooting Water Level Controller...");
  Serial.println("!!! WARNING: Do not hold D4/GPIO2 switch during boot. !!!");

  pinMode(sensor1, INPUT_PULLUP);
  pinMode(sensor2, INPUT_PULLUP);
  pinMode(sensor3, INPUT_PULLUP);
  pinMode(sensor4, INPUT_PULLUP);
  pinMode(relayPin, OUTPUT);
  pinMode(manualPin, INPUT_PULLUP); // NEW
  
  digitalWrite(relayPin, LOW); // Motor OFF by default
  lastMotorState = false;
  lastSwitchState = digitalRead(manualPin); // Init debounce logic

  Wire.begin(0, 5); // Using D3 (GPIO0) and D1 (GPIO5) for I2C
  lcd.init(); lcd.backlight();
  lcd.createChar(0, wifiOn);
  lcd.createChar(1, wifiOff);

  lcd.setCursor(6,0); lcd.print("K.B.C");
  lcd.setCursor(0,1); lcd.print("Home Automation");
  delay(2000); lcd.clear();

  lcd.setCursor(0,0); lcd.print("Water Level:");
  lcd.setCursor(0,1); lcd.print("Motor:OFF ");
  lcd.setCursor(10,1); lcd.write(0);
  lcd.setCursor(11,1); lcd.print("--:--");

  WiFi.begin(ssid, password);
  wifiStartTime = millis();

  // ---- START WEB OTA ----
  setupWebOTA();
}

void loop() {
  // --- Core Services ---
  server.handleClient();   // Handle HTTP requests for OTA
  
  if (wifiOK) {
    espalexa.loop();       // Handle Alexa requests
    
    // --- TIME FIX ---
    // Periodically call update() to re-sync time.
    // The NTPClient library is smart and will only fetch
    // from the server based on the 60-second interval.
    timeClient.update();
  }

  // --- WiFi Connection Management (non-blocking) ---
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (firstAttempt && !isConnected && (millis() - wifiStartTime >= wifiTimeout)) {
    firstAttempt = false;
    lcd.setCursor(0,1); lcd.print("WiFi Failed     ");
    delay(1000);
    lcd.setCursor(0,1); lcd.print("Motor:OFF       ");
    lcd.setCursor(10,1); lcd.write(1);
  }

  if (isConnected && !wifiOK) {
    // --- Runs ONCE on WiFi connection ---
    wifiOK = true;
    firstAttempt = false;
    
    // 1. Sync Time
    timeClient.begin();
    Serial.print("Syncing time... ");

    // --- TIME FIX ---
    // Use forceUpdate() to wait for a valid time reply.
    // This prevents the "5:30" bug.
    if (timeClient.forceUpdate()) {
        Serial.println("Success.");
        timeSynced = true;
        lastSyncMillis = millis(); // Store the *real* time of this sync
        lastNtpResync = millis();
        // Get the *actual* current time
        offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
        Serial.printf("Time set to: %s\n", timeClient.getFormattedTime().c_str());
    } else {
        Serial.println("Failed.");
        timeSynced = false; // Don't try to use the soft-RTC
    }

    // 2. Start Alexa
    espalexa.addDevice("Motor", relayChanged);
    espalexa.begin();
    Serial.println("Espalexa Started.");

    lcd.setCursor(0,1); lcd.print("WiFi Connected  ");
    delay(1000);
    lcd.setCursor(0,1); lcd.print("                ");

    Serial.print("IP: "); Serial.println(WiFi.localIP());
  }

  if (!isConnected && wifiOK) {
    // --- Runs ONCE on WiFi disconnect ---
    wifiOK = false;
    timeSynced = false;
    Serial.println("WiFi Disconnected.");
    lcd.setCursor(0,1); lcd.print("WiFi Disconnect ");
    delay(1000);
    lcd.setCursor(0,1); lcd.print("Motor:OFF       ");
    lcd.setCursor(10,1); lcd.write(1);
  }

  // --- Time Update ---
  String currentTime = "--:--";
  if (timeSynced) {
    
    // --- TIME FIX ---
    // The timeClient.update() runs in the background.
    // Let's re-read the "base" time from it every 10 minutes
    // to prevent our soft-RTC from drifting.
    if (millis() - lastNtpResync > 600000) { // 10 minutes
        lastNtpResync = millis();
        lastSyncMillis = millis();
        offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();
        Serial.println("NTP soft-RTC re-synced.");
    }

    unsigned long elapsed = (millis() - lastSyncMillis)/1000;
    unsigned long total = offsetSeconds + elapsed;
    int h = (total/3600) % 24;
    int m = (total/60) % 60;
    char buf[6];
    sprintf(buf, "%02d:%02d", h, m);
    currentTime = String(buf);
  }

  // --- LOGIC BLOCK 1: GATHER INPUTS ---

  // 1a. Manual Switch (non-blocking debounce)
  bool currentSwitchState = digitalRead(manualPin);
  if (currentSwitchState != lastSwitchState) {
    lastSwitchTime = millis(); // Reset debounce timer
  }

  if ((millis() - lastSwitchTime) > debounceDelay) {
    // State is stable
    if (currentSwitchState == LOW && lastSwitchState == HIGH) {
      // Button was just pressed
      Serial.println("Manual switch pressed. Toggling motor.");
      motorON = !motorON; // Toggle the master variable
    }
  }
  lastSwitchState = currentSwitchState;
  
  // 1b. Alexa commands (handled by espalexa.loop() at top)

  // 1c. Water Level Sensors
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

  // Update level on LCD
  lcd.setCursor(12,0); lcd.print("    "); 
  lcd.setCursor(12,0); lcd.print(level);


  // --- LOGIC BLOCK 2: APPLY RULES (Safety Overrides) ---
  if (level == "0%") {
    motorON = true; // Auto-ON (safety fill)
  }
  if (level == "100%") {
    motorON = false; // Auto-OFF (safety shutoff)
  }

  // --- LOGIC BLOCK 3: ACT ON FINAL STATE ---
  if (motorON && !lastMotorState) {
    motorTime = millis(); // Motor just turned ON
    Serial.println("Motor state changed to ON.");
  }
  if (!motorON && lastMotorState) {
    Serial.println("Motor state changed to OFF.");
  }
  lastMotorState = motorON; // Save state for next loop

  // 2. Set physical relay
  digitalWrite(relayPin, motorON ? HIGH : LOW);

  // 3. Update LCD Display (Row 1)
  lcd.setCursor(0,1);
  if (motorON) {
    // Motor is ON
    int mins = (millis() - motorTime) / 60000;
    char buf[17]; // 16 chars + null terminator
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

    lcd.setCursor(11,1); 
    lcd.print(currentTime);
  }
}

