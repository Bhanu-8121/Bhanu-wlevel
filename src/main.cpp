#include <Wire.h>

#include <LiquidCrystal_I2C.h>

#include <ESP8266WiFi.h>

#include <NTPClient.h>

#include <WiFiUdp.h>



// ===== WEB OTA =====

#include <ESP8266WebServer.h>

#include <ESP8266HTTPUpdateServer.h>



ESP8266WebServer server(80);

ESP8266HTTPUpdateServer httpUpdater;



// ===== WIFI =====

const char* ssid = "test";

const char* password = "123456789";



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

bool firstAttempt = true;

unsigned long wifiStartTime = 0;

const unsigned long wifiTimeout = 30000;



unsigned long blinkTime = 0;

bool blinkState = false;



bool motorON = false;

unsigned long motorTime = 0;



// Local RTC

bool timeSynced = false;

unsigned long lastSyncMillis = 0;

unsigned long offsetSeconds = 0;



// ===== WEB OTA SETUP (iPhone-compatible) =====

void setupWebOTA() {

  // Username = "kbc"

  // Password = "987654321"

  httpUpdater.setup(&server, "/update", "kbc", "987654321");  

  server.begin();

  Serial.println("WEB OTA Ready!");

  Serial.println("Go to: http://<ESP-IP>/update");

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

  server.handleClient();   // <==== IMPORTANT FOR WEB OTA



  bool isConnected = (WiFi.status() == WL_CONNECTED);



  if (firstAttempt && !isConnected && (millis() - wifiStartTime < wifiTimeout)) {

    if (millis() - blinkTime > 500) {

      blinkState = !blinkState;

      blinkTime = millis();

      lcd.setCursor(10,1);

      lcd.write(blinkState ? 0 : ' ');

    }

  }

  else if (firstAttempt && !isConnected && (millis() - wifiStartTime >= wifiTimeout)) {

    firstAttempt = false;

    lcd.setCursor(0,1); lcd.print("WiFi Failed     ");

    delay(1000);

    lcd.setCursor(0,1); lcd.print("Motor:OFF       ");

    lcd.setCursor(10,1); lcd.write(1);

  }



  if (isConnected && !wifiOK) {

    wifiOK = true;

    firstAttempt = false;

    timeClient.begin();

    timeClient.update();

    timeSynced = true;

    lastSyncMillis = millis();

    offsetSeconds = timeClient.getHours()*3600 + timeClient.getMinutes()*60 + timeClient.getSeconds();



    lcd.setCursor(0,1); lcd.print("WiFi Connected  ");

    delay(1000);

    lcd.setCursor(0,1); lcd.print("                ");



    Serial.print("IP: ");

    Serial.println(WiFi.localIP());

  }



  if (!isConnected && wifiOK) {

    wifiOK = false;

    lcd.setCursor(0,1); lcd.print("WiFi Disconnect ");

    delay(1000);

    lcd.setCursor(0,1); lcd.print("Motor:OFF       ");

    lcd.setCursor(10,1); lcd.write(1);

  }



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



  if (level=="0%" && !motorON){

    motorON=true; digitalWrite(relayPin,HIGH); motorTime=millis();

  }

  if (level=="100%" && motorON){

    motorON=false; digitalWrite(relayPin,LOW);

  }



  lcd.setCursor(0,1);

  if (motorON){

    int mins=(millis()-motorTime)/60000;

    char buf[16]; sprintf(buf,"Motor:ON frm %02dM",mins);

    lcd.print(buf);

  }

  else{

    lcd.print("Motor:OFF ");

    lcd.setCursor(10,1);



    if (wifiOK) lcd.write(0);

    else if (firstAttempt){

      if (millis() - blinkTime > 500) {

        blinkState = !blinkState;

        blinkTime = millis();

      }

      lcd.write(blinkState ? 0 : ' ');

    }

    else lcd.write(1);



    lcd.setCursor(11,1); lcd.print(currentTime);

  }



  delay(200);

}
