#include <Arduino.h>

void setup() {
  Serial.begin(115200);
}

void loop() {
  Serial.println("WebOTA Test Build OK");
  delay(1000);
}
