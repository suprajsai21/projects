#include <math.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>

/* ---------- MQ135 ---------- */
const int MQ135_PIN = 34;

// Calibration values
float R0 = 7.20;
const float RL = 10.0;
const float VC = 3.3;

bool calibrationMode = false;
const float FRESH_AIR_CO2 = 410.0;

// CO2 curve constants
const float CO2_A = 116.6020682;
const float CO2_B = -2.769034857;

/* ---------- OTHER MODULES ---------- */
#define PIR_PIN 27
#define FAN_PIN 12
#define BUZZER 14

#define RED_LED 25
#define GREEN_LED 26
#define BLUE_LED 33

#define DHTPIN 4
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);
Adafruit_INA219 ina219;
LiquidCrystal_I2C lcd(0x27, 16, 2);

/* ---------- INA219 ENERGY SETTINGS ---------- */
const float tariffPerkWh = 8.0;   // Rs per kWh
unsigned long previousMillis = 0;
float energy_Wh = 0.0;

/* ---------- PIR FAN TIMER SETTINGS ---------- */
unsigned long fanStartTime = 0;
const unsigned long fanRunDuration = 300000; // 5 minutes
bool fanRunning = false;

/* ---------- HELPER FUNCTIONS ---------- */
float getVoltage(int adcValue) {
  return adcValue * (3.3 / 4095.0);
}

float getRs(int adcValue) {
  float vout = getVoltage(adcValue);
  if (vout < 0.01) vout = 0.01;
  return ((VC - vout) / vout) * RL;
}

float getCO2ppm(float rs, float r0) {
  float ratio = rs / r0;
  return CO2_A * pow(ratio, CO2_B);
}

float calculateR0(float rs, float ppm) {
  return rs / pow((ppm / CO2_A), (1.0 / CO2_B));
}

/* ---------- SETUP ---------- */
void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);

  pinMode(PIR_PIN, INPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);

  // Safe startup states
  digitalWrite(FAN_PIN, LOW);
  digitalWrite(BUZZER, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BLUE_LED, LOW);

  Wire.begin(21, 22);   // ESP32 SDA, SCL

  dht.begin();

  if (!ina219.begin()) {
    Serial.println("INA219 not found");
    while (1) {
      delay(10);
    }
  }

  lcd.init();
  lcd.backlight();
  lcd.clear();

  previousMillis = millis();

  Serial.println("======================================");
  Serial.println("SMART ENVIRONMENT MONITOR - ESP32");
  Serial.println("======================================");

  if (calibrationMode) {
    Serial.println("Mode: CALIBRATION");
    Serial.println("Place sensor in fresh outdoor air");
  } else {
    Serial.println("Mode: MEASUREMENT");
  }
  Serial.println();
}

/* ---------- LOOP ---------- */
void loop() {
  int adcValue = analogRead(MQ135_PIN);

  float voltage = getVoltage(adcValue);
  float rs = getRs(adcValue);

  /* ---------- CALIBRATION MODE ---------- */
  if (calibrationMode) {
    float estimatedR0 = calculateR0(rs, FRESH_AIR_CO2);

    Serial.println("----------- AIR REPORT -----------");
    Serial.print("ADC Value       : ");
    Serial.println(adcValue);
    Serial.print("Sensor Voltage  : ");
    Serial.print(voltage, 3);
    Serial.println(" V");
    Serial.print("Sensor Rs       : ");
    Serial.print(rs, 3);
    Serial.println(" kOhm");
    Serial.print("Estimated R0    : ");
    Serial.print(estimatedR0, 3);
    Serial.println(" kOhm");
    Serial.println("----------------------------------");
    Serial.println("Copy average R0 after stable values");
    Serial.println();

    delay(2000);
    return;
  }

  /* ---------- CO2 ---------- */
  float co2ppm = getCO2ppm(rs, R0);

  if (co2ppm < 350) co2ppm = 350;
  if (co2ppm > 5000) co2ppm = 5000;

  /* ---------- AQI ---------- */
  int AQI = map(adcValue, 0, 4095, 0, 500);

  String airQuality;
  float nh3ppb = 0.0;
  float benzeneppb = 0.0;
  float coppm = 0.0;

  if (AQI <= 25) {
    airQuality = "Excellent";
    nh3ppb = 1.0;
    benzeneppb = 0.3;
    coppm = 0.05;
  }
  else if (AQI <= 50) {
    airQuality = "Good";
    nh3ppb = 2.0;
    benzeneppb = 0.5;
    coppm = 0.10;
  }
  else if (AQI <= 75) {
    airQuality = "Fair";
    nh3ppb = 5.0;
    benzeneppb = 1.0;
    coppm = 0.20;
  }
  else if (AQI <= 100) {
    airQuality = "Moderate";
    nh3ppb = 10.0;
    benzeneppb = 2.0;
    coppm = 0.30;
  }
  else if (AQI <= 125) {
    airQuality = "Slightly Polluted";
    nh3ppb = 18.0;
    benzeneppb = 3.0;
    coppm = 0.50;
  }
  else if (AQI <= 150) {
    airQuality = "Unhealthy Sensitive";
    nh3ppb = 30.0;
    benzeneppb = 4.0;
    coppm = 0.80;
  }
  else if (AQI <= 175) {
    airQuality = "Mildly Unhealthy";
    nh3ppb = 45.0;
    benzeneppb = 5.5;
    coppm = 1.10;
  }
  else if (AQI <= 200) {
    airQuality = "Unhealthy";
    nh3ppb = 60.0;
    benzeneppb = 7.0;
    coppm = 1.50;
  }
  else if (AQI <= 225) {
    airQuality = "Very Unhealthy";
    nh3ppb = 80.0;
    benzeneppb = 8.5;
    coppm = 2.00;
  }
  else if (AQI <= 250) {
    airQuality = "Heavily Polluted";
    nh3ppb = 100.0;
    benzeneppb = 10.0;
    coppm = 2.50;
  }
  else if (AQI <= 275) {
    airQuality = "Severe";
    nh3ppb = 120.0;
    benzeneppb = 12.0;
    coppm = 3.00;
  }
  else if (AQI <= 300) {
    airQuality = "Very Severe";
    nh3ppb = 140.0;
    benzeneppb = 14.0;
    coppm = 3.50;
  }
  else if (AQI <= 325) {
    airQuality = "Hazardous L1";
    nh3ppb = 165.0;
    benzeneppb = 16.0;
    coppm = 4.00;
  }
  else if (AQI <= 350) {
    airQuality = "Hazardous L2";
    nh3ppb = 190.0;
    benzeneppb = 18.0;
    coppm = 4.50;
  }
  else if (AQI <= 375) {
    airQuality = "Hazardous L3";
    nh3ppb = 215.0;
    benzeneppb = 20.0;
    coppm = 5.00;
  }
  else if (AQI <= 400) {
    airQuality = "Extreme Hazard";
    nh3ppb = 240.0;
    benzeneppb = 22.0;
    coppm = 5.50;
  }
  else if (AQI <= 425) {
    airQuality = "Extreme Hazard+";
    nh3ppb = 270.0;
    benzeneppb = 24.0;
    coppm = 6.00;
  }
  else if (AQI <= 450) {
    airQuality = "Critical Pollution";
    nh3ppb = 300.0;
    benzeneppb = 26.0;
    coppm = 6.50;
  }
  else if (AQI <= 475) {
    airQuality = "Critical Pollution+";
    nh3ppb = 330.0;
    benzeneppb = 28.0;
    coppm = 7.00;
  }
  else {
    airQuality = "Emergency";
    nh3ppb = 360.0;
    benzeneppb = 30.0;
    coppm = 8.00;
  }

  /* ---------- CO2 STATUS ---------- */
  String co2Status;
  if (co2ppm <= 450) {
    co2Status = "Fresh Outdoor Air";
  }
  else if (co2ppm <= 600) {
    co2Status = "Very Good Indoor Air";
  }
  else if (co2ppm <= 800) {
    co2Status = "Normal Indoor Air";
  }
  else if (co2ppm <= 1000) {
    co2Status = "Acceptable";
  }
  else if (co2ppm <= 1200) {
    co2Status = "Poor Ventilation";
  }
  else if (co2ppm <= 2000) {
    co2Status = "High CO2 Level";
  }
  else {
    co2Status = "Very High CO2 Level";
  }

  /* ---------- PIR + FAN TIMER LOGIC ---------- */
  int pirValue = digitalRead(PIR_PIN);
  int occupancy_status = 0;

  if (pirValue == HIGH) {
    occupancy_status = 1;
    fanStartTime = millis();   // restart timer every new detection
    fanRunning = true;
  }

  if (fanRunning) {
    if (millis() - fanStartTime < fanRunDuration) {
      digitalWrite(FAN_PIN, HIGH);
    } else {
      digitalWrite(FAN_PIN, LOW);
      fanRunning = false;
    }
  } else {
    digitalWrite(FAN_PIN, LOW);
  }

  /* ---------- RGB LED + BUZZER ---------- */
  digitalWrite(BUZZER, LOW);

  if (AQI <= 50) {
    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(RED_LED, LOW);
    digitalWrite(BLUE_LED, LOW);
  }
  else if (AQI <= 100) {
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, LOW);
    digitalWrite(BLUE_LED, HIGH);
  }
  else {
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(BLUE_LED, LOW);
    digitalWrite(RED_LED, HIGH);

    digitalWrite(BUZZER, HIGH);
    delay(200);
    digitalWrite(BUZZER, LOW);
  }

  /* ---------- DHT ---------- */
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  if (isnan(temperature)) temperature = 0;
  if (isnan(humidity)) humidity = 0;

  /* ---------- INA219 ADVANCED ---------- */
  unsigned long currentMillis = millis();
  float elapsedHours = (currentMillis - previousMillis) / 3600000.0;
  previousMillis = currentMillis;

  float busVoltage = ina219.getBusVoltage_V();
  float shuntVoltage = ina219.getShuntVoltage_mV();
  float current_mA = ina219.getCurrent_mA();
  float power_mW = ina219.getPower_mW();

  float power_W = power_mW / 1000.0;

  energy_Wh += power_W * elapsedHours;
  float energy_kWh = energy_Wh / 1000.0;

  float cost = energy_kWh * tariffPerkWh;

  /* ---------- SERIAL REPORT ---------- */
  Serial.println("----------- AIR + ENERGY REPORT -----------");

  Serial.print("ADC Value       : ");
  Serial.println(adcValue);

  Serial.print("Sensor Voltage  : ");
  Serial.print(voltage, 3);
  Serial.println(" V");

  Serial.print("Sensor Rs       : ");
  Serial.print(rs, 3);
  Serial.println(" kOhm");

  Serial.print("AQI             : ");
  Serial.println(AQI);

  Serial.print("Air Quality     : ");
  Serial.println(airQuality);

  Serial.print("Approx CO2      : ");
  Serial.print(co2ppm, 1);
  Serial.println(" ppm");

  Serial.print("CO2 Status      : ");
  Serial.println(co2Status);

  Serial.print("Approx NH3      : ");
  Serial.print(nh3ppb, 1);
  Serial.println(" ppb");

  Serial.print("Approx Benzene  : ");
  Serial.print(benzeneppb, 1);
  Serial.println(" ppb");

  Serial.print("Approx CO       : ");
  Serial.print(coppm, 2);
  Serial.println(" ppm");

  Serial.print("Temp            : ");
  Serial.print(temperature);
  Serial.println(" C");

  Serial.print("Humidity        : ");
  Serial.print(humidity);
  Serial.println(" %");

  Serial.print("Bus Voltage     : ");
  Serial.print(busVoltage, 2);
  Serial.println(" V");

  Serial.print("Shunt Voltage   : ");
  Serial.print(shuntVoltage, 2);
  Serial.println(" mV");

  Serial.print("Current         : ");
  Serial.print(current_mA, 2);
  Serial.println(" mA");

  Serial.print("Power           : ");
  Serial.print(power_W, 3);
  Serial.println(" W");

  Serial.print("Energy          : ");
  Serial.print(energy_Wh, 4);
  Serial.println(" Wh");

  Serial.print("Energy          : ");
  Serial.print(energy_kWh, 6);
  Serial.println(" kWh");

  Serial.print("Cost            : Rs ");
  Serial.println(cost, 4);

  Serial.print("Occupancy       : ");
  Serial.println(occupancy_status);

  Serial.print("Fan Status      : ");
  if (digitalRead(FAN_PIN))
    Serial.println("ON");
  else
    Serial.println("OFF");

  Serial.println("-------------------------------------------");
  Serial.println();

  /* ---------- LCD ---------- */
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("AQI:");
  lcd.print(AQI);

  lcd.setCursor(8, 0);
  lcd.print("T:");
  lcd.print(temperature, 0);

  lcd.setCursor(0, 1);
  lcd.print("CO2:");
  lcd.print((int)co2ppm);

  lcd.setCursor(10, 1);
  if (occupancy_status == 1)
    lcd.print("Y");
  else
    lcd.print("N");

  delay(2000);
}