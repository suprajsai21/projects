#include <Wire.h>
#include <Adafruit_INA219.h>

Adafruit_INA219 ina219;

// -------- USER SETTINGS --------
const float tariffPerkWh = 8.0;        // Rs per kWh
const float batteryVoltage = 12.0;     // Battery voltage
const float batteryAh = 100.0;         // Battery capacity in Ah
const float inverterEfficiency = 0.85; // 85% efficiency
// -------------------------------

unsigned long previousMillis = 0;
float energy_Wh = 0.0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22);   // ESP32 SDA, SCL

  if (!ina219.begin()) {
    Serial.println("INA219 not found");
    while (1) {
      delay(10);
    }
  }

  previousMillis = millis();

  Serial.println("====================================");
  Serial.println("INA219 Full Monitoring Started");
  Serial.println("Voltage, Current, Power, Energy, Cost, Backup");
  Serial.println("====================================");
}

void loop() {
  unsigned long currentMillis = millis();
  float elapsedHours = (currentMillis - previousMillis) / 3600000.0;
  previousMillis = currentMillis;

  // Read INA219 values
  float busVoltage = ina219.getBusVoltage_V();        // V
  float shuntVoltage_mV = ina219.getShuntVoltage_mV();// mV
  float current_mA = ina219.getCurrent_mA();          // mA
  float power_mW = ina219.getPower_mW();              // mW

  // Load voltage = bus voltage + shunt drop
  float loadVoltage = busVoltage + (shuntVoltage_mV / 1000.0);

  // Convert power to watts
  float power_W = power_mW / 1000.0;

  // Energy calculation
  energy_Wh += power_W * elapsedHours;
  float energy_kWh = energy_Wh / 1000.0;

  // Cost calculation
  float cost = energy_kWh * tariffPerkWh;

  // Battery backup calculation
  float usableBatteryWh = batteryVoltage * batteryAh * inverterEfficiency;
  float backupHours = 0.0;

  if (power_W > 0.01) {
    backupHours = usableBatteryWh / power_W;
  }

  Serial.println("====================================");

  Serial.print("Bus Voltage      : ");
  Serial.print(busVoltage, 2);
  Serial.println(" V");

  Serial.print("Shunt Voltage    : ");
  Serial.print(shuntVoltage_mV, 2);
  Serial.println(" mV");

  Serial.print("Load Voltage     : ");
  Serial.print(loadVoltage, 2);
  Serial.println(" V");

  Serial.print("Current          : ");
  Serial.print(current_mA, 2);
  Serial.println(" mA");

  Serial.print("Power            : ");
  Serial.print(power_W, 3);
  Serial.println(" W");

  Serial.print("Energy Used      : ");
  Serial.print(energy_Wh, 4);
  Serial.println(" Wh");

  Serial.print("Energy Used      : ");
  Serial.print(energy_kWh, 6);
  Serial.println(" kWh");

  Serial.print("Estimated Cost   : Rs ");
  Serial.println(cost, 4);

  Serial.print("Battery Capacity : ");
  Serial.print(usableBatteryWh, 2);
  Serial.println(" Wh");

  if (power_W > 0.01) {
    Serial.print("Backup Time      : ");
    Serial.print(backupHours, 2);
    Serial.println(" hours");
  } else {
    Serial.println("Backup Time      : No load detected");
  }

  Serial.println("====================================");
  delay(2000);
}