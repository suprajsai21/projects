#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// LCD address 0x27 is common
LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {

  Serial.begin(115200);

  // Start I2C
  Wire.begin(21, 22);

  // Initialize LCD
  lcd.init();
  lcd.backlight();

  lcd.setCursor(0,0);
  lcd.print("LCD WORKING");

  lcd.setCursor(0,1);
  lcd.print("ESP32 TEST");
}

void loop() {

}
