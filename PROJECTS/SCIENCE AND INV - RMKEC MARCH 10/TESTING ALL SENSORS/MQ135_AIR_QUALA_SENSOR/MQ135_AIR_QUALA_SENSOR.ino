#include <math.h>

const int MQ135_PIN = 34;

// ---------- Calibration values ----------
float R0 = 7.20;         // Replace with your calibrated R0 later
const float RL = 10.0;   // Load resistance in kOhm
const float VC = 3.3;    // ESP32 ADC voltage

bool calibrationMode = false;
const float FRESH_AIR_CO2 = 410.0;

// ---------- CO2 curve constants ----------
const float CO2_A = 116.6020682;
const float CO2_B = -2.769034857;

// ---------- Helper functions ----------
float getVoltage(int adcValue) {
  return adcValue * (3.3 / 4095.0);
}

float getRs(int adcValue) {
  float vout = getVoltage(adcValue);

  if (vout < 0.01) vout = 0.01;   // avoid divide by zero

  return ((VC - vout) / vout) * RL;
}

float getCO2ppm(float rs, float r0) {
  float ratio = rs / r0;
  return CO2_A * pow(ratio, CO2_B);
}

float calculateR0(float rs, float ppm) {
  return rs / pow((ppm / CO2_A), (1.0 / CO2_B));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  analogReadResolution(12);

  Serial.println("======================================");
  Serial.println("MQ135 Air Quality Monitor - ESP32");
  Serial.println("======================================");

  if (calibrationMode) {
    Serial.println("Mode: CALIBRATION");
    Serial.println("Place sensor in fresh outdoor air");
  } else {
    Serial.println("Mode: MEASUREMENT");
  }

  Serial.println();
}

void loop() {
  int adcValue = analogRead(MQ135_PIN);
  float voltage = getVoltage(adcValue);
  float rs = getRs(adcValue);

  Serial.println("----------- AIR REPORT -----------");
  Serial.print("ADC Value       : ");
  Serial.println(adcValue);

  Serial.print("Sensor Voltage  : ");
  Serial.print(voltage, 3);
  Serial.println(" V");

  Serial.print("Sensor Rs       : ");
  Serial.print(rs, 3);
  Serial.println(" kOhm");

  if (calibrationMode) {
    float estimatedR0 = calculateR0(rs, FRESH_AIR_CO2);
    Serial.print("Estimated R0    : ");
    Serial.print(estimatedR0, 3);
    Serial.println(" kOhm");
    Serial.println("----------------------------------");
    Serial.println("Copy average R0 after stable values");
    Serial.println();
    delay(2000);
    return;
  }

  // Approximate CO2 using Rs/R0
  float co2ppm = getCO2ppm(rs, R0);

  // Limit unrealistic values a bit for display
  if (co2ppm < 350) co2ppm = 350;
  if (co2ppm > 5000) co2ppm = 5000;

  // AQI-style mapping from ADC
  int AQI = map(adcValue, 0, 4095, 0, 500);

  String airQuality;
  float nh3ppb;
  float benzeneppb;
  float coppm;

  if (AQI <= 50) {
    airQuality = "Good";
    nh3ppb = 2.0;
    benzeneppb = 0.5;
    coppm = 0.1;
  }
  else if (AQI <= 100) {
    airQuality = "Moderate";
    nh3ppb = 10.0;
    benzeneppb = 2.0;
    coppm = 0.3;
  }
  else if (AQI <= 150) {
    airQuality = "Unhealthy for Sensitive";
    nh3ppb = 30.0;
    benzeneppb = 4.0;
    coppm = 0.8;
  }
  else if (AQI <= 200) {
    airQuality = "Unhealthy";
    nh3ppb = 60.0;
    benzeneppb = 7.0;
    coppm = 1.5;
  }
  else if (AQI <= 300) {
    airQuality = "Very Unhealthy";
    nh3ppb = 120.0;
    benzeneppb = 12.0;
    coppm = 3.0;
  }
  else {
    airQuality = "Hazardous";
    nh3ppb = 250.0;
    benzeneppb = 20.0;
    coppm = 5.0;
  }

  // Additional CO2-based status
  String co2Status;
  if (co2ppm <= 450) {
    co2Status = "Fresh Outdoor Air";
  }
  else if (co2ppm <= 800) {
    co2Status = "Normal Indoor Air";
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
  Serial.print(coppm, 1);
  Serial.println(" ppm");

  Serial.println("----------------------------------");
  Serial.println();

  delay(10000);
}