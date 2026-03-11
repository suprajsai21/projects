#include <math.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

/* =========================================================
   SMART ENVIRONMENT MONITOR - CORE CODE V2 + WIFI POST + BSSID
   Features:
   - smoothing
   - non-blocking timing
   - AUTO / MANUAL occupancy mode
   - error handling
   - CSV logging
   - saved R0 in Preferences
   - WiFi data push to laptop server
   - BSSID locked WiFi connection

   MODE LOGIC:
   AUTO   -> PIR controls occupancy
   MANUAL -> occupancy forced = 1

   AIR CONTROL LOGIC:
   occupancy = 1 and AQI > 100  -> AIR CONTROL ON
   occupancy = 0 and AQI > 200  -> AIR CONTROL ON
   else                         -> AIR CONTROL OFF
   ========================================================= */

/* ---------- WIFI ---------- */
const char* ssid = "ESP";
const char* password = "12345678";

/* Replace with your router BSSID */
const uint8_t bssid[6] = {0xC6,0x31,0x37,0xD9,0x09,0xC6};

/* Replace with your laptop IP and Flask route */
const char* serverUrl = "http://10.76.154.13:5001/data";

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
#define BUZZER_PIN 14

#define RED_LED 25
#define GREEN_LED 26
#define BLUE_LED 33

#define DHTPIN 4
#define DHTTYPE DHT11

/* ---------- OBJECTS ---------- */
DHT dht(DHTPIN, DHTTYPE);
Adafruit_INA219 ina219;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences preferences;

/* ---------- ENERGY ---------- */
const float tariffPerkWh = 8.0;
float energy_Wh = 0.0;

/* ---------- PIR HOLD ---------- */
unsigned long lastMotionTime = 0;
const unsigned long occupancyDelay = 120000; // 120 sec
unsigned long lastMotionAgeSec = 0;

/* ---------- TIMING ---------- */
unsigned long lastSensorRead = 0;
unsigned long lastLCDUpdate = 0;
unsigned long lastCSVLog = 0;
unsigned long lastBuzzerToggle = 0;
unsigned long lastServerSend = 0;
unsigned long lastWiFiRetry = 0;
unsigned long systemStartMillis = 0;

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long LCD_INTERVAL = 2000;
const unsigned long CSV_INTERVAL = 2000;
const unsigned long SERVER_SEND_INTERVAL = 2000;
const unsigned long WIFI_RETRY_INTERVAL = 10000;

/* ---------- MODES ---------- */
enum ControlMode { AUTO_MODE, MANUAL_MODE };
ControlMode controlMode = AUTO_MODE;

/* ---------- SENSOR ERROR FLAGS ---------- */
bool dhtError = false;
bool inaError = false;
bool mqError = false;

/* ---------- LIVE DATA ---------- */
int rawAdcValue = 0;
float rawVoltage = 0.0;
float rawRs = 0.0;

int AQI = 0;
String airQuality = "Unknown";
float co2ppm = 0.0;
String co2Status = "Unknown";
float nh3ppb = 0.0;
float benzeneppb = 0.0;
float coppm = 0.0;

float temperature = 0.0;
float humidity = 0.0;

float busVoltage = 0.0;
float shuntVoltage = 0.0;
float current_mA = 0.0;
float power_W = 0.0;
float energy_kWh = 0.0;
float cost = 0.0;

int occupancy_status = 0;
String airCtrlStatus = "OFF";

unsigned long previousEnergyMillis = 0;
unsigned long readingCount = 0;

/* ---------- SMOOTHING ---------- */
const int FILTER_SIZE = 10;

int adcBuffer[FILTER_SIZE];
float tempBuffer[FILTER_SIZE];
float humBuffer[FILTER_SIZE];
float currentBuffer[FILTER_SIZE];
float powerBuffer[FILTER_SIZE];
bool filterFilled = false;
int filterIndex = 0;

/* ---------- LCD PAGE ---------- */
int lcdPage = 0;

/* ---------- BUZZER ---------- */
bool buzzerState = false;
int buzzerPatternMs = 0;
bool buzzerEnabled = false;

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

void initFilters() {
  for (int i = 0; i < FILTER_SIZE; i++) {
    adcBuffer[i] = 0;
    tempBuffer[i] = 0;
    humBuffer[i] = 0;
    currentBuffer[i] = 0;
    powerBuffer[i] = 0;
  }
}

void updateFilters(int adcVal, float tempVal, float humVal, float currVal, float powerVal) {
  adcBuffer[filterIndex] = adcVal;
  tempBuffer[filterIndex] = tempVal;
  humBuffer[filterIndex] = humVal;
  currentBuffer[filterIndex] = currVal;
  powerBuffer[filterIndex] = powerVal;

  filterIndex++;
  if (filterIndex >= FILTER_SIZE) {
    filterIndex = 0;
    filterFilled = true;
  }
}

int getValidCount() {
  return filterFilled ? FILTER_SIZE : filterIndex;
}

float averageIntBuffer(int* arr) {
  int count = getValidCount();
  if (count == 0) return 0;
  long sum = 0;
  for (int i = 0; i < count; i++) sum += arr[i];
  return (float)sum / count;
}

float averageFloatBuffer(float* arr) {
  int count = getValidCount();
  if (count == 0) return 0;
  float sum = 0;
  for (int i = 0; i < count; i++) sum += arr[i];
  return sum / count;
}

void loadSavedR0() {
  preferences.begin("envmon", true);
  if (preferences.isKey("R0")) {
    R0 = preferences.getFloat("R0", 7.20);
  }
  preferences.end();
}

void saveR0(float value) {
  preferences.begin("envmon", false);
  preferences.putFloat("R0", value);
  preferences.end();
  R0 = value;
}

String getModeText() {
  return (controlMode == AUTO_MODE) ? "AUTO" : "MANUAL";
}

void setAirQualityLevels(int aqi) {
  nh3ppb = 0.0;
  benzeneppb = 0.0;
  coppm = 0.0;

  if (aqi <= 25) {
    airQuality = "Excellent";
    nh3ppb = 1.0; benzeneppb = 0.3; coppm = 0.05;
  }
  else if (aqi <= 50) {
    airQuality = "Good";
    nh3ppb = 2.0; benzeneppb = 0.5; coppm = 0.10;
  }
  else if (aqi <= 75) {
    airQuality = "Fair";
    nh3ppb = 5.0; benzeneppb = 1.0; coppm = 0.20;
  }
  else if (aqi <= 100) {
    airQuality = "Moderate";
    nh3ppb = 10.0; benzeneppb = 2.0; coppm = 0.30;
  }
  else if (aqi <= 125) {
    airQuality = "Slightly Polluted";
    nh3ppb = 18.0; benzeneppb = 3.0; coppm = 0.50;
  }
  else if (aqi <= 150) {
    airQuality = "Unhealthy Sensitive";
    nh3ppb = 30.0; benzeneppb = 4.0; coppm = 0.80;
  }
  else if (aqi <= 175) {
    airQuality = "Mildly Unhealthy";
    nh3ppb = 45.0; benzeneppb = 5.5; coppm = 1.10;
  }
  else if (aqi <= 200) {
    airQuality = "Unhealthy";
    nh3ppb = 60.0; benzeneppb = 7.0; coppm = 1.50;
  }
  else if (aqi <= 225) {
    airQuality = "Very Unhealthy";
    nh3ppb = 80.0; benzeneppb = 8.5; coppm = 2.00;
  }
  else if (aqi <= 250) {
    airQuality = "Heavily Polluted";
    nh3ppb = 100.0; benzeneppb = 10.0; coppm = 2.50;
  }
  else if (aqi <= 275) {
    airQuality = "Severe";
    nh3ppb = 120.0; benzeneppb = 12.0; coppm = 3.00;
  }
  else if (aqi <= 300) {
    airQuality = "Very Severe";
    nh3ppb = 140.0; benzeneppb = 14.0; coppm = 3.50;
  }
  else if (aqi <= 325) {
    airQuality = "Hazardous L1";
    nh3ppb = 165.0; benzeneppb = 16.0; coppm = 4.00;
  }
  else if (aqi <= 350) {
    airQuality = "Hazardous L2";
    nh3ppb = 190.0; benzeneppb = 18.0; coppm = 4.50;
  }
  else if (aqi <= 375) {
    airQuality = "Hazardous L3";
    nh3ppb = 215.0; benzeneppb = 20.0; coppm = 5.00;
  }
  else if (aqi <= 400) {
    airQuality = "Extreme Hazard";
    nh3ppb = 240.0; benzeneppb = 22.0; coppm = 5.50;
  }
  else if (aqi <= 425) {
    airQuality = "Extreme Hazard+";
    nh3ppb = 270.0; benzeneppb = 24.0; coppm = 6.00;
  }
  else if (aqi <= 450) {
    airQuality = "Critical Pollution";
    nh3ppb = 300.0; benzeneppb = 26.0; coppm = 6.50;
  }
  else if (aqi <= 475) {
    airQuality = "Critical Pollution+";
    nh3ppb = 330.0; benzeneppb = 28.0; coppm = 7.00;
  }
  else {
    airQuality = "Emergency";
    nh3ppb = 360.0; benzeneppb = 30.0; coppm = 8.00;
  }
}

void setCO2Status(float co2) {
  if (co2 <= 450) co2Status = "Fresh Outdoor Air";
  else if (co2 <= 600) co2Status = "Very Good Indoor Air";
  else if (co2 <= 800) co2Status = "Normal Indoor Air";
  else if (co2 <= 1000) co2Status = "Acceptable";
  else if (co2 <= 1200) co2Status = "Poor Ventilation";
  else if (co2 <= 2000) co2Status = "High CO2 Level";
  else co2Status = "Very High CO2 Level";
}

void updateOccupancy() {
  if (controlMode == MANUAL_MODE) {
    occupancy_status = 1;
    lastMotionAgeSec = 0;
    return;
  }

  int pirValue = digitalRead(PIR_PIN);
  if (pirValue == HIGH) {
    lastMotionTime = millis();
  }

  if (millis() - lastMotionTime < occupancyDelay) occupancy_status = 1;
  else occupancy_status = 0;

  lastMotionAgeSec = (millis() - lastMotionTime) / 1000;
}

void updateFanLogic() {
  if (occupancy_status == 1 && AQI > 100) {
    digitalWrite(FAN_PIN, HIGH);
  }
  else if (occupancy_status == 0 && AQI > 200) {
    digitalWrite(FAN_PIN, HIGH);
  }
  else {
    digitalWrite(FAN_PIN, LOW);
  }

  airCtrlStatus = digitalRead(FAN_PIN) ? "ON" : "OFF";
}

void updateLEDLogic() {
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
  }
}

void updateBuzzerPattern() {
  if (AQI <= 50) buzzerPatternMs = 50;
  else if (AQI <= 100) buzzerPatternMs = 150;
  else if (AQI <= 200) buzzerPatternMs = 300;
  else buzzerPatternMs = 500;

  buzzerEnabled = true;
}

void handleBuzzerNonBlocking() {
  if (!buzzerEnabled) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
    return;
  }

  unsigned long now = millis();

  if (!buzzerState) {
    if (now - lastBuzzerToggle >= 2000) {
      digitalWrite(BUZZER_PIN, HIGH);
      buzzerState = true;
      lastBuzzerToggle = now;
    }
  } else {
    if (now - lastBuzzerToggle >= (unsigned long)buzzerPatternMs) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerState = false;
      lastBuzzerToggle = now;
    }
  }
}

void readSensors() {
  rawAdcValue = analogRead(MQ135_PIN);
  rawVoltage = getVoltage(rawAdcValue);
  rawRs = getRs(rawAdcValue);

  mqError = (rawAdcValue < 0 || rawAdcValue > 4095);

  float rawTemp = dht.readTemperature();
  float rawHum = dht.readHumidity();

  if (isnan(rawTemp) || isnan(rawHum)) {
    dhtError = true;
    rawTemp = temperature;
    rawHum = humidity;
  } else {
    dhtError = false;
  }

  float rawBusVoltage = ina219.getBusVoltage_V();
  float rawShuntVoltage = ina219.getShuntVoltage_mV();
  float rawCurrent = ina219.getCurrent_mA();
  float rawPowerW = ina219.getPower_mW() / 1000.0;

  if (isnan(rawBusVoltage) || isnan(rawCurrent) || isnan(rawPowerW)) {
    inaError = true;
    rawBusVoltage = busVoltage;
    rawShuntVoltage = shuntVoltage;
    rawCurrent = current_mA;
    rawPowerW = power_W;
  } else {
    inaError = false;
  }

  updateFilters(rawAdcValue, rawTemp, rawHum, rawCurrent, rawPowerW);

  rawAdcValue = (int)averageIntBuffer(adcBuffer);
  temperature = averageFloatBuffer(tempBuffer);
  humidity = averageFloatBuffer(humBuffer);
  current_mA = averageFloatBuffer(currentBuffer);
  power_W = averageFloatBuffer(powerBuffer);

  rawVoltage = getVoltage(rawAdcValue);
  rawRs = getRs(rawAdcValue);
  co2ppm = getCO2ppm(rawRs, R0);

  if (co2ppm < 350) co2ppm = 350;
  if (co2ppm > 5000) co2ppm = 5000;

  AQI = map(rawAdcValue, 0, 4095, 0, 500);

  busVoltage = rawBusVoltage;
  shuntVoltage = rawShuntVoltage;

  setAirQualityLevels(AQI);
  setCO2Status(co2ppm);

  unsigned long now = millis();
  float elapsedHours = (now - previousEnergyMillis) / 3600000.0;
  previousEnergyMillis = now;

  energy_Wh += power_W * elapsedHours;
  energy_kWh = energy_Wh / 1000.0;
  cost = energy_kWh * tariffPerkWh;

  readingCount++;
}

void sendDataToServer() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<768> doc;
  doc["time_s"] = (millis() - systemStartMillis) / 1000;
  doc["aqi"] = AQI;
  doc["airQuality"] = airQuality;
  doc["co2"] = co2ppm;
  doc["co2Status"] = co2Status;
  doc["temp"] = temperature;
  doc["humidity"] = humidity;
  doc["nh3"] = nh3ppb;
  doc["benzene"] = benzeneppb;
  doc["co"] = coppm;
  doc["voltage"] = busVoltage;
  doc["current"] = current_mA;
  doc["power"] = power_W;
  doc["energy"] = energy_kWh;
  doc["cost"] = cost;
  doc["occupancy"] = occupancy_status;
  doc["airCtrl"] = digitalRead(FAN_PIN);
  doc["mode"] = getModeText();
  doc["dhtErr"] = dhtError;
  doc["inaErr"] = inaError;
  doc["mqErr"] = mqError;

  String jsonBody;
  serializeJson(doc, jsonBody);

  int httpCode = http.POST(jsonBody);

  Serial.print("POST Code       : ");
  Serial.println(httpCode);

  http.end();
}

void printDetailedStatus() {
  Serial.println("----------- AIR + ENERGY REPORT -----------");
  Serial.print("Mode            : "); Serial.println(getModeText());
  Serial.print("Reading Count   : "); Serial.println(readingCount);
  Serial.print("Uptime (sec)    : "); Serial.println((millis() - systemStartMillis) / 1000);

  Serial.print("ADC Value       : "); Serial.println(rawAdcValue);
  Serial.print("Sensor Voltage  : "); Serial.print(rawVoltage, 3); Serial.println(" V");
  Serial.print("Sensor Rs       : "); Serial.print(rawRs, 3); Serial.println(" kOhm");
  Serial.print("Saved R0        : "); Serial.println(R0, 3);

  Serial.print("AQI             : "); Serial.println(AQI);
  Serial.print("Air Quality     : "); Serial.println(airQuality);
  Serial.print("Approx CO2      : "); Serial.print(co2ppm, 1); Serial.println(" ppm");
  Serial.print("CO2 Status      : "); Serial.println(co2Status);
  Serial.print("Approx NH3      : "); Serial.print(nh3ppb, 1); Serial.println(" ppb");
  Serial.print("Approx Benzene  : "); Serial.print(benzeneppb, 1); Serial.println(" ppb");
  Serial.print("Approx CO       : "); Serial.print(coppm, 2); Serial.println(" ppm");

  Serial.print("Temp            : "); Serial.print(temperature, 1); Serial.println(" C");
  Serial.print("Humidity        : "); Serial.print(humidity, 1); Serial.println(" %");

  Serial.print("Bus Voltage     : "); Serial.print(busVoltage, 2); Serial.println(" V");
  Serial.print("Shunt Voltage   : "); Serial.print(shuntVoltage, 2); Serial.println(" mV");
  Serial.print("Current         : "); Serial.print(current_mA, 2); Serial.println(" mA");
  Serial.print("Power           : "); Serial.print(power_W, 3); Serial.println(" W");
  Serial.print("Energy Wh       : "); Serial.print(energy_Wh, 4); Serial.println(" Wh");
  Serial.print("Energy          : "); Serial.print(energy_kWh, 6); Serial.println(" kWh");
  Serial.print("Cost            : Rs "); Serial.println(cost, 4);

  Serial.print("Occupancy       : "); Serial.println(occupancy_status);
  Serial.print("Last Motion Age : "); Serial.print(lastMotionAgeSec); Serial.println(" sec");
  Serial.print("Air Control     : "); Serial.println(airCtrlStatus);

  Serial.print("DHT Error       : "); Serial.println(dhtError ? "YES" : "NO");
  Serial.print("INA219 Error    : "); Serial.println(inaError ? "YES" : "NO");
  Serial.print("MQ Error        : "); Serial.println(mqError ? "YES" : "NO");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi IP         : "); Serial.println(WiFi.localIP());
    Serial.print("WiFi BSSID      : "); Serial.println(WiFi.BSSIDstr());
  } else {
    Serial.println("WiFi            : DISCONNECTED");
  }

  Serial.println("-------------------------------------------");
  Serial.println();
}

void printCSVHeader() {
  Serial.println("CSV_HEADER,time_s,aqi,co2,temp,humidity,nh3,benzene,co,busVoltage,current_mA,power_W,energy_kWh,cost,occupancy,airCtrl,mode,dhtErr,inaErr,mqErr");
}

void printCSVLine() {
  Serial.print("CSV,");
  Serial.print((millis() - systemStartMillis) / 1000); Serial.print(",");
  Serial.print(AQI); Serial.print(",");
  Serial.print(co2ppm, 1); Serial.print(",");
  Serial.print(temperature, 1); Serial.print(",");
  Serial.print(humidity, 1); Serial.print(",");
  Serial.print(nh3ppb, 1); Serial.print(",");
  Serial.print(benzeneppb, 1); Serial.print(",");
  Serial.print(coppm, 2); Serial.print(",");
  Serial.print(busVoltage, 2); Serial.print(",");
  Serial.print(current_mA, 2); Serial.print(",");
  Serial.print(power_W, 3); Serial.print(",");
  Serial.print(energy_kWh, 6); Serial.print(",");
  Serial.print(cost, 4); Serial.print(",");
  Serial.print(occupancy_status); Serial.print(",");
  Serial.print(digitalRead(FAN_PIN)); Serial.print(",");
  Serial.print(getModeText()); Serial.print(",");
  Serial.print(dhtError); Serial.print(",");
  Serial.print(inaError); Serial.print(",");
  Serial.println(mqError);
}

void updateLCD() {
  lcd.clear();

  if (dhtError || inaError || mqError) {
    lcd.setCursor(0, 0);
    lcd.print("SENSOR ERROR");
    lcd.setCursor(0, 1);
    if (dhtError) lcd.print("DHT ");
    if (inaError) lcd.print("INA ");
    if (mqError) lcd.print("MQ ");
    return;
  }

  if (lcdPage == 0) {
    lcd.setCursor(0, 0);
    lcd.print("AQI:");
    lcd.print(AQI);

    lcd.setCursor(8, 0);
    lcd.print("T:");
    lcd.print(temperature, 0);

    lcd.setCursor(0, 1);
    lcd.print("CO2:");
    lcd.print((int)co2ppm);

    lcd.setCursor(11, 1);
    lcd.print(occupancy_status ? "Y" : "N");
  }
  else if (lcdPage == 1) {
    lcd.setCursor(0, 0);
    lcd.print("I:");
    lcd.print(current_mA, 0);
    lcd.print("mA");

    lcd.setCursor(0, 1);
    lcd.print("P:");
    lcd.print(power_W, 1);
    lcd.print("W");
  }
  else {
    lcd.setCursor(0, 0);
    lcd.print("AirCtrl:");
    lcd.print(airCtrlStatus);

    lcd.setCursor(0, 1);
    lcd.print(getModeText());
    lcd.print(" Rs:");
    lcd.print(cost, 2);
  }

  lcdPage++;
  if (lcdPage > 2) lcdPage = 0;
}

void handleSerialCommands() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "HELP") {
    Serial.println("Commands:");
    Serial.println("HELP");
    Serial.println("STATUS");
    Serial.println("AUTO");
    Serial.println("MANUAL");
    Serial.println("SHOW_R0");
    Serial.println("SAVE_R0=7.20");
    Serial.println("CAL_ON");
    Serial.println("CAL_OFF");
  }
  else if (cmd == "STATUS") {
    printDetailedStatus();
  }
  else if (cmd == "AUTO") {
    controlMode = AUTO_MODE;
    Serial.println("Mode set to AUTO (PIR control)");
  }
  else if (cmd == "MANUAL") {
    controlMode = MANUAL_MODE;
    Serial.println("Mode set to MANUAL (Occupancy forced = 1)");
  }
  else if (cmd == "SHOW_R0") {
    Serial.print("Current saved R0 = ");
    Serial.println(R0, 3);
  }
  else if (cmd.startsWith("SAVE_R0=")) {
    float newR0 = cmd.substring(8).toFloat();
    if (newR0 > 0.1 && newR0 < 1000.0) {
      saveR0(newR0);
      Serial.print("R0 saved: ");
      Serial.println(R0, 3);
    } else {
      Serial.println("Invalid R0 value");
    }
  }
  else if (cmd == "CAL_ON") {
    calibrationMode = true;
    Serial.println("Calibration mode ON");
  }
  else if (cmd == "CAL_OFF") {
    calibrationMode = false;
    Serial.println("Calibration mode OFF");
  }
  else {
    Serial.println("Unknown command. Type HELP");
  }
}

void handleCalibrationMode() {
  int adcValue = analogRead(MQ135_PIN);
  float voltage = getVoltage(adcValue);
  float rs = getRs(adcValue);
  float estimatedR0 = calculateR0(rs, FRESH_AIR_CO2);

  Serial.println("----------- CALIBRATION -----------");
  Serial.print("ADC Value       : "); Serial.println(adcValue);
  Serial.print("Sensor Voltage  : "); Serial.print(voltage, 3); Serial.println(" V");
  Serial.print("Sensor Rs       : "); Serial.print(rs, 3); Serial.println(" kOhm");
  Serial.print("Estimated R0    : "); Serial.print(estimatedR0, 3); Serial.println(" kOhm");
  Serial.println("Use SAVE_R0=<value> to store");
  Serial.println("-----------------------------------");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("CAL MODE");
  lcd.setCursor(0, 1);
  lcd.print("R0:");
  lcd.print(estimatedR0, 2);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password, 0, bssid, true);

  Serial.print("Connecting to WiFi (BSSID locked)");

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("Connected BSSID: ");
    Serial.println(WiFi.BSSIDstr());
  } else {
    Serial.println("WiFi connection failed");
  }
}

/* ---------- SETUP ---------- */
void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);

  pinMode(PIR_PIN, INPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);

  digitalWrite(FAN_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BLUE_LED, LOW);

  Wire.begin(21, 22);
  dht.begin();

  lcd.init();
  lcd.backlight();
  lcd.clear();

  if (!ina219.begin()) {
    Serial.println("INA219 not found");
    inaError = true;
  } else {
    inaError = false;
  }

  loadSavedR0();
  initFilters();

  systemStartMillis = millis();
  previousEnergyMillis = millis();

  Serial.println("======================================");
  Serial.println("SMART ENVIRONMENT MONITOR - CORE V2");
  Serial.println("======================================");
  Serial.print("Loaded R0: ");
  Serial.println(R0, 3);
  Serial.println("Mode: MEASUREMENT");
  Serial.println("Type HELP in Serial Monitor");
  Serial.println();

  connectWiFi();
  printCSVHeader();

  lcd.setCursor(0, 0);
  lcd.print("Smart Env V2");
  lcd.setCursor(0, 1);
  lcd.print("R0:");
  lcd.print(R0, 2);
  delay(1500);
  lcd.clear();
}

/* ---------- LOOP ---------- */
void loop() {
  handleSerialCommands();

  unsigned long now = millis();

  if (calibrationMode) {
    if (now - lastSensorRead >= SENSOR_INTERVAL) {
      lastSensorRead = now;
      handleCalibrationMode();
    }
    return;
  }

  if (WiFi.status() != WL_CONNECTED && now - lastWiFiRetry >= WIFI_RETRY_INTERVAL) {
    lastWiFiRetry = now;
    connectWiFi();
  }

  updateOccupancy();

  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    readSensors();
    updateFanLogic();
    updateLEDLogic();
    updateBuzzerPattern();
    printDetailedStatus();
  }

  if (now - lastCSVLog >= CSV_INTERVAL) {
    lastCSVLog = now;
    printCSVLine();
  }

  if (now - lastServerSend >= SERVER_SEND_INTERVAL) {
    lastServerSend = now;
    sendDataToServer();
  }

  if (now - lastLCDUpdate >= LCD_INTERVAL) {
    lastLCDUpdate = now;
    updateLCD();
  }

  handleBuzzerNonBlocking();
}