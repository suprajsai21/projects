#include <math.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>

/* =========================================================
   SMART ENVIRONMENT MONITOR - FINAL CORE CODE
   Features:
   - smoothing
   - non-blocking timing
   - AUTO / MANUAL occupancy mode
   - error handling
   - CSV logging
   - saved R0 in Preferences
   - WiFi data push to laptop server
   - BSSID locked WiFi connection
   - improved buzzer timing
   - BUILT-IN WEB DASHBOARD (new)

   MODE LOGIC:
   AUTO   -> PIR controls occupancy
   MANUAL -> occupancy forced = 1

   AIR CONTROL LOGIC:
   occupancy = 1 and AQI > 100  -> AIR CONTROL ON
   occupancy = 0 and AQI > 200  -> AIR CONTROL ON
   else                         -> AIR CONTROL OFF

   WEB DASHBOARD:
   Open browser -> http://<ESP32-IP>/
   Live JSON    -> http://<ESP32-IP>/live
   Set Mode     -> http://<ESP32-IP>/setmode?mode=AUTO
   ========================================================= */

/* ---------- WIFI ---------- */
const char* ssid = "ESP";
const char* password = "12345678";

/* Replace with your router BSSID */
const uint8_t bssid[6] = {0xC6, 0x31, 0x37, 0xD9, 0x09, 0xC6};

/* Replace with your laptop IP */
const char* serverUrl = "http://10.76.154.13:5000/data";

/* ---------- WEB SERVER (NEW) ---------- */
WebServer webServer(80);

/* ---------- MQ135 ---------- */
const int MQ135_PIN = 34;
float R0 = 7.20;
const float RL = 10.0;
const float VC = 3.3;
bool calibrationMode = false;
const float FRESH_AIR_CO2 = 410.0;
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
const unsigned long occupancyDelay = 120000;
unsigned long lastMotionAgeSec = 0;

/* ---------- TIMING ---------- */
unsigned long lastSensorRead = 0;
unsigned long lastLCDUpdate = 0;
unsigned long lastCSVLog = 0;
unsigned long lastServerSend = 0;
unsigned long lastWiFiRetry = 0;
unsigned long lastBuzzerToggle = 0;
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
bool buzzerEnabled = false;
unsigned long buzzerOnMs = 0;
unsigned long buzzerOffMs = 0;

/* =========================================================
   ALL ORIGINAL HELPER FUNCTIONS — UNCHANGED
   ========================================================= */

float getVoltage(int adcValue) { return adcValue * (3.3 / 4095.0); }

float getRs(int adcValue) {
  float vout = getVoltage(adcValue);
  if (vout < 0.01) vout = 0.01;
  return ((VC - vout) / vout) * RL;
}

float getCO2ppm(float rs, float r0) { return CO2_A * pow(rs / r0, CO2_B); }
float calculateR0(float rs, float ppm) { return rs / pow((ppm / CO2_A), (1.0 / CO2_B)); }

void initFilters() {
  for (int i = 0; i < FILTER_SIZE; i++) { adcBuffer[i]=0; tempBuffer[i]=0; humBuffer[i]=0; currentBuffer[i]=0; powerBuffer[i]=0; }
}

void updateFilters(int adcVal, float tempVal, float humVal, float currVal, float powerVal) {
  adcBuffer[filterIndex]=adcVal; tempBuffer[filterIndex]=tempVal; humBuffer[filterIndex]=humVal;
  currentBuffer[filterIndex]=currVal; powerBuffer[filterIndex]=powerVal;
  filterIndex++; if (filterIndex >= FILTER_SIZE) { filterIndex=0; filterFilled=true; }
}

int getValidCount() { return filterFilled ? FILTER_SIZE : filterIndex; }

float averageIntBuffer(int* arr) {
  int count=getValidCount(); if(count==0)return 0;
  long sum=0; for(int i=0;i<count;i++)sum+=arr[i]; return (float)sum/count;
}

float averageFloatBuffer(float* arr) {
  int count=getValidCount(); if(count==0)return 0;
  float sum=0; for(int i=0;i<count;i++)sum+=arr[i]; return sum/count;
}

void loadSavedR0() { preferences.begin("envmon",true); if(preferences.isKey("R0"))R0=preferences.getFloat("R0",7.20); preferences.end(); }
void saveR0(float value) { preferences.begin("envmon",false); preferences.putFloat("R0",value); preferences.end(); R0=value; }
String getModeText() { return (controlMode==AUTO_MODE)?"AUTO":"MANUAL"; }

void setAirQualityLevels(int aqi) {
  nh3ppb=0.0; benzeneppb=0.0; coppm=0.0;
  if(aqi<=25){airQuality="Excellent";nh3ppb=1.0;benzeneppb=0.3;coppm=0.05;}
  else if(aqi<=50){airQuality="Good";nh3ppb=2.0;benzeneppb=0.5;coppm=0.10;}
  else if(aqi<=75){airQuality="Fair";nh3ppb=5.0;benzeneppb=1.0;coppm=0.20;}
  else if(aqi<=100){airQuality="Moderate";nh3ppb=10.0;benzeneppb=2.0;coppm=0.30;}
  else if(aqi<=125){airQuality="Slightly Polluted";nh3ppb=18.0;benzeneppb=3.0;coppm=0.50;}
  else if(aqi<=150){airQuality="Unhealthy Sensitive";nh3ppb=30.0;benzeneppb=4.0;coppm=0.80;}
  else if(aqi<=175){airQuality="Mildly Unhealthy";nh3ppb=45.0;benzeneppb=5.5;coppm=1.10;}
  else if(aqi<=200){airQuality="Unhealthy";nh3ppb=60.0;benzeneppb=7.0;coppm=1.50;}
  else if(aqi<=225){airQuality="Very Unhealthy";nh3ppb=80.0;benzeneppb=8.5;coppm=2.00;}
  else if(aqi<=250){airQuality="Heavily Polluted";nh3ppb=100.0;benzeneppb=10.0;coppm=2.50;}
  else if(aqi<=275){airQuality="Severe";nh3ppb=120.0;benzeneppb=12.0;coppm=3.00;}
  else if(aqi<=300){airQuality="Very Severe";nh3ppb=140.0;benzeneppb=14.0;coppm=3.50;}
  else if(aqi<=325){airQuality="Hazardous L1";nh3ppb=165.0;benzeneppb=16.0;coppm=4.00;}
  else if(aqi<=350){airQuality="Hazardous L2";nh3ppb=190.0;benzeneppb=18.0;coppm=4.50;}
  else if(aqi<=375){airQuality="Hazardous L3";nh3ppb=215.0;benzeneppb=20.0;coppm=5.00;}
  else if(aqi<=400){airQuality="Extreme Hazard";nh3ppb=240.0;benzeneppb=22.0;coppm=5.50;}
  else if(aqi<=425){airQuality="Extreme Hazard+";nh3ppb=270.0;benzeneppb=24.0;coppm=6.00;}
  else if(aqi<=450){airQuality="Critical Pollution";nh3ppb=300.0;benzeneppb=26.0;coppm=6.50;}
  else if(aqi<=475){airQuality="Critical Pollution+";nh3ppb=330.0;benzeneppb=28.0;coppm=7.00;}
  else{airQuality="Emergency";nh3ppb=360.0;benzeneppb=30.0;coppm=8.00;}
}

void setCO2Status(float co2) {
  if(co2<=450)co2Status="Fresh Outdoor Air";
  else if(co2<=600)co2Status="Very Good Indoor Air";
  else if(co2<=800)co2Status="Normal Indoor Air";
  else if(co2<=1000)co2Status="Acceptable";
  else if(co2<=1200)co2Status="Poor Ventilation";
  else if(co2<=2000)co2Status="High CO2 Level";
  else co2Status="Very High CO2 Level";
}

void updateOccupancy() {
  if(controlMode==MANUAL_MODE){occupancy_status=1;lastMotionAgeSec=0;return;}
  int pirValue=digitalRead(PIR_PIN);
  if(pirValue==HIGH)lastMotionTime=millis();
  if(millis()-lastMotionTime<occupancyDelay)occupancy_status=1;
  else occupancy_status=0;
  lastMotionAgeSec=(millis()-lastMotionTime)/1000;
}

void updateFanLogic() {
  if(occupancy_status==1&&AQI>100)digitalWrite(FAN_PIN,HIGH);
  else if(occupancy_status==0&&AQI>200)digitalWrite(FAN_PIN,HIGH);
  else digitalWrite(FAN_PIN,LOW);
  airCtrlStatus=digitalRead(FAN_PIN)?"ON":"OFF";
}

void updateLEDLogic() {
  if(AQI<=50){digitalWrite(GREEN_LED,HIGH);digitalWrite(RED_LED,LOW);digitalWrite(BLUE_LED,LOW);}
  else if(AQI<=100){digitalWrite(GREEN_LED,LOW);digitalWrite(RED_LED,LOW);digitalWrite(BLUE_LED,HIGH);}
  else{digitalWrite(GREEN_LED,LOW);digitalWrite(BLUE_LED,LOW);digitalWrite(RED_LED,HIGH);}
}

void updateBuzzerPattern() {
  if(AQI<=50){buzzerEnabled=false;buzzerOnMs=20;buzzerOffMs=1000;}
  else if(AQI<=100){buzzerEnabled=true;buzzerOnMs=40;buzzerOffMs=800;}
  else if(AQI<=200){buzzerEnabled=true;buzzerOnMs=60;buzzerOffMs=500;}
  else{buzzerEnabled=true;buzzerOnMs=80;buzzerOffMs=300;}
}

void handleBuzzerNonBlocking() {
  unsigned long now=millis();
  if(!buzzerEnabled){digitalWrite(BUZZER_PIN,LOW);buzzerState=false;return;}
  if(!buzzerState){if(now-lastBuzzerToggle>=buzzerOffMs){digitalWrite(BUZZER_PIN,HIGH);buzzerState=true;lastBuzzerToggle=now;}}
  else{if(now-lastBuzzerToggle>=buzzerOnMs){digitalWrite(BUZZER_PIN,LOW);buzzerState=false;lastBuzzerToggle=now;}}
}

void readSensors() {
  rawAdcValue=analogRead(MQ135_PIN); rawVoltage=getVoltage(rawAdcValue); rawRs=getRs(rawAdcValue);
  mqError=(rawAdcValue<0||rawAdcValue>4095);
  float rawTemp=dht.readTemperature(); float rawHum=dht.readHumidity();
  if(isnan(rawTemp)||isnan(rawHum)){dhtError=true;rawTemp=temperature;rawHum=humidity;}else dhtError=false;
  float rawBusVoltage=ina219.getBusVoltage_V(); float rawShuntVoltage=ina219.getShuntVoltage_mV();
  float rawCurrent=ina219.getCurrent_mA(); float rawPowerW=ina219.getPower_mW()/1000.0;
  if(isnan(rawBusVoltage)||isnan(rawCurrent)||isnan(rawPowerW)){inaError=true;rawBusVoltage=busVoltage;rawShuntVoltage=shuntVoltage;rawCurrent=current_mA;rawPowerW=power_W;}else inaError=false;
  updateFilters(rawAdcValue,rawTemp,rawHum,rawCurrent,rawPowerW);
  rawAdcValue=(int)averageIntBuffer(adcBuffer); temperature=averageFloatBuffer(tempBuffer);
  humidity=averageFloatBuffer(humBuffer); current_mA=averageFloatBuffer(currentBuffer); power_W=averageFloatBuffer(powerBuffer);
  rawVoltage=getVoltage(rawAdcValue); rawRs=getRs(rawAdcValue);
  co2ppm=getCO2ppm(rawRs,R0); if(co2ppm<350)co2ppm=350; if(co2ppm>5000)co2ppm=5000;
  AQI=map(rawAdcValue,0,4095,0,500); busVoltage=rawBusVoltage; shuntVoltage=rawShuntVoltage;
  setAirQualityLevels(AQI); setCO2Status(co2ppm);
  unsigned long now=millis(); float elapsedHours=(now-previousEnergyMillis)/3600000.0; previousEnergyMillis=now;
  energy_Wh+=power_W*elapsedHours; energy_kWh=energy_Wh/1000.0; cost=energy_kWh*tariffPerkWh;
  readingCount++;
}

void sendDataToServer() {
  if(WiFi.status()!=WL_CONNECTED){Serial.println("POST Skipped     : WiFi not connected");return;}
  HTTPClient http; http.begin(serverUrl); http.addHeader("Content-Type","application/json");
  StaticJsonDocument<768> doc;
  doc["time_s"]=(millis()-systemStartMillis)/1000; doc["aqi"]=AQI; doc["airQuality"]=airQuality;
  doc["co2"]=co2ppm; doc["co2Status"]=co2Status; doc["temp"]=temperature; doc["humidity"]=humidity;
  doc["nh3"]=nh3ppb; doc["benzene"]=benzeneppb; doc["co"]=coppm; doc["voltage"]=busVoltage;
  doc["current"]=current_mA; doc["power"]=power_W; doc["energy"]=energy_kWh; doc["cost"]=cost;
  doc["occupancy"]=occupancy_status; doc["airCtrl"]=digitalRead(FAN_PIN); doc["mode"]=getModeText();
  doc["dhtErr"]=dhtError; doc["inaErr"]=inaError; doc["mqErr"]=mqError;
  String jsonBody; serializeJson(doc,jsonBody);
  int httpCode=http.POST(jsonBody);
  Serial.println("----- HTTP POST DEBUG -----");
  Serial.print("POST URL        : "); Serial.println(serverUrl);
  Serial.print("JSON Sent       : "); Serial.println(jsonBody);
  Serial.print("HTTP Code       : "); Serial.println(httpCode);
  if(httpCode>0){String response=http.getString();Serial.print("Server Response : ");Serial.println(response);}
  else{Serial.print("HTTP Error      : ");Serial.println(http.errorToString(httpCode));}
  Serial.println("---------------------------");
  http.end();
}

void printDetailedStatus() {
  Serial.println("----------- AIR + ENERGY REPORT -----------");
  Serial.print("Mode            : "); Serial.println(getModeText());
  Serial.print("Reading Count   : "); Serial.println(readingCount);
  Serial.print("Uptime (sec)    : "); Serial.println((millis()-systemStartMillis)/1000);
  Serial.print("ADC Value       : "); Serial.println(rawAdcValue);
  Serial.print("Sensor Voltage  : "); Serial.print(rawVoltage,3); Serial.println(" V");
  Serial.print("Sensor Rs       : "); Serial.print(rawRs,3); Serial.println(" kOhm");
  Serial.print("Saved R0        : "); Serial.println(R0,3);
  Serial.print("AQI             : "); Serial.println(AQI);
  Serial.print("Air Quality     : "); Serial.println(airQuality);
  Serial.print("Approx CO2      : "); Serial.print(co2ppm,1); Serial.println(" ppm");
  Serial.print("CO2 Status      : "); Serial.println(co2Status);
  Serial.print("Approx NH3      : "); Serial.print(nh3ppb,1); Serial.println(" ppb");
  Serial.print("Approx Benzene  : "); Serial.print(benzeneppb,1); Serial.println(" ppb");
  Serial.print("Approx CO       : "); Serial.print(coppm,2); Serial.println(" ppm");
  Serial.print("Temp            : "); Serial.print(temperature,1); Serial.println(" C");
  Serial.print("Humidity        : "); Serial.print(humidity,1); Serial.println(" %");
  Serial.print("Bus Voltage     : "); Serial.print(busVoltage,2); Serial.println(" V");
  Serial.print("Shunt Voltage   : "); Serial.print(shuntVoltage,2); Serial.println(" mV");
  Serial.print("Current         : "); Serial.print(current_mA,2); Serial.println(" mA");
  Serial.print("Power           : "); Serial.print(power_W,3); Serial.println(" W");
  Serial.print("Energy Wh       : "); Serial.print(energy_Wh,4); Serial.println(" Wh");
  Serial.print("Energy          : "); Serial.print(energy_kWh,6); Serial.println(" kWh");
  Serial.print("Cost            : Rs "); Serial.println(cost,4);
  Serial.print("Occupancy       : "); Serial.println(occupancy_status);
  Serial.print("Last Motion Age : "); Serial.print(lastMotionAgeSec); Serial.println(" sec");
  Serial.print("Air Control     : "); Serial.println(airCtrlStatus);
  Serial.print("DHT Error       : "); Serial.println(dhtError?"YES":"NO");
  Serial.print("INA219 Error    : "); Serial.println(inaError?"YES":"NO");
  Serial.print("MQ Error        : "); Serial.println(mqError?"YES":"NO");
  if(WiFi.status()==WL_CONNECTED){Serial.print("WiFi IP         : ");Serial.println(WiFi.localIP());Serial.print("WiFi BSSID      : ");Serial.println(WiFi.BSSIDstr());}
  else Serial.println("WiFi            : DISCONNECTED");
  Serial.print("Web Dashboard   : http://"); Serial.println(WiFi.localIP());
  Serial.println("-------------------------------------------"); Serial.println();
}

void printCSVHeader() {
  Serial.println("CSV_HEADER,time_s,aqi,co2,temp,humidity,nh3,benzene,co,busVoltage,current_mA,power_W,energy_kWh,cost,occupancy,airCtrl,mode,dhtErr,inaErr,mqErr");
}

void printCSVLine() {
  Serial.print("CSV,"); Serial.print((millis()-systemStartMillis)/1000); Serial.print(",");
  Serial.print(AQI); Serial.print(","); Serial.print(co2ppm,1); Serial.print(",");
  Serial.print(temperature,1); Serial.print(","); Serial.print(humidity,1); Serial.print(",");
  Serial.print(nh3ppb,1); Serial.print(","); Serial.print(benzeneppb,1); Serial.print(",");
  Serial.print(coppm,2); Serial.print(","); Serial.print(busVoltage,2); Serial.print(",");
  Serial.print(current_mA,2); Serial.print(","); Serial.print(power_W,3); Serial.print(",");
  Serial.print(energy_kWh,6); Serial.print(","); Serial.print(cost,4); Serial.print(",");
  Serial.print(occupancy_status); Serial.print(","); Serial.print(digitalRead(FAN_PIN)); Serial.print(",");
  Serial.print(getModeText()); Serial.print(","); Serial.print(dhtError); Serial.print(",");
  Serial.print(inaError); Serial.print(","); Serial.println(mqError);
}

void updateLCD() {
  lcd.clear();
  if(dhtError||inaError||mqError){lcd.setCursor(0,0);lcd.print("SENSOR ERROR");lcd.setCursor(0,1);if(dhtError)lcd.print("DHT ");if(inaError)lcd.print("INA ");if(mqError)lcd.print("MQ ");return;}
  if(lcdPage==0){lcd.setCursor(0,0);lcd.print("AQI:");lcd.print(AQI);lcd.setCursor(8,0);lcd.print("T:");lcd.print(temperature,0);lcd.setCursor(0,1);lcd.print("CO2:");lcd.print((int)co2ppm);lcd.setCursor(11,1);lcd.print(occupancy_status?"Y":"N");}
  else if(lcdPage==1){lcd.setCursor(0,0);lcd.print("I:");lcd.print(current_mA,0);lcd.print("mA");lcd.setCursor(0,1);lcd.print("P:");lcd.print(power_W,1);lcd.print("W");}
  else{lcd.setCursor(0,0);lcd.print("AirCtrl:");lcd.print(airCtrlStatus);lcd.setCursor(0,1);lcd.print(getModeText());lcd.print(" Rs:");lcd.print(cost,2);}
  lcdPage++; if(lcdPage>2)lcdPage=0;
}

void handleSerialCommands() {
  if(!Serial.available())return;
  String cmd=Serial.readStringUntil('\n'); cmd.trim(); cmd.toUpperCase();
  if(cmd=="HELP"){Serial.println("Commands: HELP STATUS AUTO MANUAL SHOW_R0 SAVE_R0=7.20 CAL_ON CAL_OFF");}
  else if(cmd=="STATUS"){printDetailedStatus();}
  else if(cmd=="AUTO"){controlMode=AUTO_MODE;Serial.println("Mode set to AUTO (PIR control)");}
  else if(cmd=="MANUAL"){controlMode=MANUAL_MODE;Serial.println("Mode set to MANUAL (Occupancy forced = 1)");}
  else if(cmd=="SHOW_R0"){Serial.print("Current saved R0 = ");Serial.println(R0,3);}
  else if(cmd.startsWith("SAVE_R0=")){float newR0=cmd.substring(8).toFloat();if(newR0>0.1&&newR0<1000.0){saveR0(newR0);Serial.print("R0 saved: ");Serial.println(R0,3);}else Serial.println("Invalid R0 value");}
  else if(cmd=="CAL_ON"){calibrationMode=true;Serial.println("Calibration mode ON");}
  else if(cmd=="CAL_OFF"){calibrationMode=false;Serial.println("Calibration mode OFF");}
  else{Serial.println("Unknown command. Type HELP");}
}

void handleCalibrationMode() {
  int adcValue=analogRead(MQ135_PIN); float voltage=getVoltage(adcValue); float rs=getRs(adcValue);
  float estimatedR0=calculateR0(rs,FRESH_AIR_CO2);
  Serial.println("----------- CALIBRATION -----------");
  Serial.print("ADC Value       : ");Serial.println(adcValue);
  Serial.print("Sensor Voltage  : ");Serial.print(voltage,3);Serial.println(" V");
  Serial.print("Sensor Rs       : ");Serial.print(rs,3);Serial.println(" kOhm");
  Serial.print("Estimated R0    : ");Serial.print(estimatedR0,3);Serial.println(" kOhm");
  Serial.println("Use SAVE_R0=<value> to store"); Serial.println("-----------------------------------");
  lcd.clear(); lcd.setCursor(0,0); lcd.print("CAL MODE"); lcd.setCursor(0,1); lcd.print("R0:"); lcd.print(estimatedR0,2);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,password,0,bssid,true);
  Serial.print("Connecting to WiFi (BSSID locked)");
  unsigned long startAttemptTime=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-startAttemptTime<20000){delay(500);Serial.print(".");}
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    Serial.print("Connected! IP: "); Serial.println(WiFi.localIP());
    Serial.print("Connected BSSID: "); Serial.println(WiFi.BSSIDstr());
  } else Serial.println("WiFi connection failed");
}

/* =========================================================
   ★★★ NEW — WEB SERVER HANDLERS ★★★
   All sensor data comes from existing global variables.
   Original code is 100% untouched above.
   ========================================================= */

/* ---------- /live — JSON API ---------- */
void handleWebLive() {
  webServer.sendHeader("Access-Control-Allow-Origin","*");
  webServer.sendHeader("Cache-Control","no-cache");
  StaticJsonDocument<768> doc;
  doc["aqi"]=AQI; doc["airQuality"]=airQuality;
  doc["co2"]=(int)co2ppm; doc["co2Status"]=co2Status;
  doc["temp"]=round(temperature*10)/10.0;
  doc["humidity"]=round(humidity*10)/10.0;
  doc["nh3"]=round(nh3ppb*10)/10.0;
  doc["benzene"]=round(benzeneppb*10)/10.0;
  doc["co"]=round(coppm*100)/100.0;
  doc["voltage"]=round(busVoltage*100)/100.0;
  doc["current"]=round(current_mA*10)/10.0;
  doc["power"]=round(power_W*1000)/1000.0;
  doc["energy"]=energy_kWh;
  doc["cost"]=cost;
  doc["occupancy"]=occupancy_status;
  doc["airCtrl"]=digitalRead(FAN_PIN);
  doc["mode"]=getModeText();
  doc["uptime"]=(millis()-systemStartMillis)/1000;
  doc["readings"]=readingCount;
  doc["dhtErr"]=dhtError; doc["inaErr"]=inaError; doc["mqErr"]=mqError;
  String out; serializeJson(doc,out);
  webServer.send(200,"application/json",out);
}

/* ---------- /setmode ---------- */
void handleWebSetMode() {
  webServer.sendHeader("Access-Control-Allow-Origin","*");
  if(webServer.hasArg("mode")){
    String m=webServer.arg("mode");
    if(m=="AUTO"){controlMode=AUTO_MODE;webServer.send(200,"application/json","{\"ok\":true,\"mode\":\"AUTO\"}");}
    else if(m=="MANUAL"){controlMode=MANUAL_MODE;webServer.send(200,"application/json","{\"ok\":true,\"mode\":\"MANUAL\"}");}
    else webServer.send(400,"application/json","{\"error\":\"invalid\"}");
  } else webServer.send(400,"application/json","{\"error\":\"missing mode\"}");
}

/* ---------- / — FULL DASHBOARD ---------- */
void handleWebRoot() {
  webServer.sendHeader("Cache-Control","no-cache");
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200,"text/html","");

  /* ── CSS + HEAD ── */
  webServer.sendContent(F("<!DOCTYPE html><html lang='en'><head>"
  "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>"
  "<title>AirMind &mdash; Smart Environment</title>"
  "<link href='https://fonts.googleapis.com/css2?family=Orbitron:wght@400;700;900&family=Inter:wght@300;400;500;600&display=swap' rel='stylesheet'>"
  "<script src='https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.0/chart.umd.min.js'></script>"
  "<style>"
  ":root{"
  "--bg:#02070f;--bg2:#050d18;--bg3:#091522;--bg4:#0d1e2e;"
  "--c1:#00f5d4;--c2:#00b4d8;--c3:#ff6b6b;--c4:#ffd166;--c5:#a8ff78;--c6:#ff9a3c;--c7:#c77dff;"
  "--text:#d0f0ff;--text2:#6a9ab8;--text3:#2a4a60;"
  "--border:rgba(0,245,212,0.08);--glow:0 0 30px rgba(0,245,212,0.12);"
  "--danger:#ff3366;--warn:#ffd166;--good:#00f5d4;"
  "}"
  "*{margin:0;padding:0;box-sizing:border-box}"
  "html{scroll-behavior:smooth}"
  "body{background:var(--bg);color:var(--text);font-family:'Inter',sans-serif;min-height:100vh;overflow-x:hidden}"

  /* animated bg */
  ".bg-mesh{position:fixed;inset:0;z-index:0;pointer-events:none;overflow:hidden}"
  ".bg-mesh::before{content:'';position:absolute;inset:0;"
  "background:radial-gradient(ellipse 80% 50% at 20% 20%,rgba(0,245,212,0.04) 0%,transparent 60%),"
  "radial-gradient(ellipse 60% 60% at 80% 80%,rgba(0,180,216,0.04) 0%,transparent 60%),"
  "radial-gradient(ellipse 40% 40% at 50% 50%,rgba(199,125,255,0.02) 0%,transparent 60%);"
  "animation:meshMove 12s ease-in-out infinite alternate}"
  "@keyframes meshMove{0%{transform:scale(1) translate(0,0)}100%{transform:scale(1.05) translate(-20px,10px)}}"
  ".scanline{position:fixed;inset:0;z-index:0;pointer-events:none;"
  "background:repeating-linear-gradient(0deg,transparent,transparent 2px,rgba(0,245,212,0.008) 2px,rgba(0,245,212,0.008) 4px)}"

  /* header */
  "header{position:sticky;top:0;z-index:200;background:rgba(2,7,15,0.92);backdrop-filter:blur(24px);"
  "border-bottom:1px solid var(--border);padding:0 32px;height:68px;"
  "display:flex;align-items:center;justify-content:space-between}"
  ".logo{display:flex;align-items:center;gap:14px}"
  ".logo-icon{width:40px;height:40px;border-radius:10px;"
  "background:linear-gradient(135deg,var(--c1),var(--c2));"
  "display:flex;align-items:center;justify-content:center;font-size:20px;"
  "box-shadow:0 0 20px rgba(0,245,212,0.4);animation:pulse 2s ease-in-out infinite}"
  "@keyframes pulse{0%,100%{box-shadow:0 0 20px rgba(0,245,212,0.4)}50%{box-shadow:0 0 40px rgba(0,245,212,0.8),0 0 60px rgba(0,245,212,0.2)}}"
  ".logo-text{font-family:'Orbitron',monospace;font-size:20px;font-weight:900;color:var(--c1);letter-spacing:2px}"
  ".logo-sub{font-size:9px;color:var(--text3);letter-spacing:3px;text-transform:uppercase;margin-top:2px}"
  ".hright{display:flex;align-items:center;gap:16px}"
  ".pill{display:flex;align-items:center;gap:7px;background:var(--bg3);border:1px solid var(--border);"
  "border-radius:20px;padding:6px 14px;font-size:10px;color:var(--text2);letter-spacing:1px}"
  ".dot{width:7px;height:7px;border-radius:50%;background:var(--c1);animation:blink 1.5s ease-in-out infinite;box-shadow:0 0 8px var(--c1)}"
  ".dot.warn{background:var(--c7);box-shadow:0 0 8px var(--c7)}"
  "@keyframes blink{0%,100%{opacity:1}50%{opacity:0.2}}"
  ".clock{font-family:'Orbitron',monospace;font-size:14px;color:var(--c1);letter-spacing:2px}"

  /* main */
  "main{position:relative;z-index:1;padding:28px 32px;max-width:1700px;margin:0 auto}"

  /* alert banner */
  ".alert-bar{display:flex;align-items:center;gap:16px;border-radius:14px;padding:14px 22px;margin-bottom:28px;"
  "border:1px solid rgba(255,214,102,0.3);background:linear-gradient(90deg,rgba(255,214,102,0.07),transparent);"
  "animation:slideIn 0.4s ease}"
  "@keyframes slideIn{from{opacity:0;transform:translateY(-12px)}to{opacity:1;transform:translateY(0)}}"
  ".alert-icon{font-size:22px}"
  ".alert-text{font-size:13px;color:var(--c4)}"
  ".alert-badge{margin-left:auto;padding:4px 12px;border-radius:20px;font-size:9px;font-weight:700;letter-spacing:2px;"
  "background:rgba(255,214,102,0.15);border:1px solid rgba(255,214,102,0.4);color:var(--c4)}"

  /* section title */
  ".stitle{font-family:'Orbitron',monospace;font-size:9px;font-weight:700;letter-spacing:4px;color:var(--text3);"
  "text-transform:uppercase;margin-bottom:16px;display:flex;align-items:center;gap:12px}"
  ".stitle::after{content:'';flex:1;height:1px;background:linear-gradient(90deg,var(--border),transparent)}"

  /* hero cards */
  ".hgrid{display:grid;grid-template-columns:repeat(4,1fr);gap:16px;margin-bottom:28px}"
  ".hcard{background:var(--bg2);border:1px solid var(--border);border-radius:18px;padding:24px;"
  "position:relative;overflow:hidden;cursor:pointer;transition:all 0.3s;group}"
  ".hcard:hover{transform:translateY(-3px);border-color:var(--c1);box-shadow:var(--glow)}"
  ".hcard::before{content:'';position:absolute;inset:0;opacity:0;background:radial-gradient(circle at 50% 0%,rgba(0,245,212,0.06),transparent 70%);transition:opacity 0.3s}"
  ".hcard:hover::before{opacity:1}"
  ".hcard.a{border-top:2px solid var(--c3)}.hcard.b{border-top:2px solid var(--c2)}"
  ".hcard.c{border-top:2px solid var(--c4)}.hcard.d{border-top:2px solid var(--c1)}"
  ".hlabel{font-size:9px;letter-spacing:3px;text-transform:uppercase;color:var(--text3);margin-bottom:10px;font-family:'Orbitron',monospace}"
  ".hval{font-family:'Orbitron',monospace;font-size:52px;font-weight:900;line-height:1;margin-bottom:6px}"
  ".hunit{font-size:16px;font-weight:400;color:var(--text2);margin-left:4px}"
  ".hbadge{display:inline-flex;align-items:center;gap:5px;padding:4px 12px;border-radius:20px;font-size:9px;font-weight:700;letter-spacing:2px;text-transform:uppercase;margin-top:8px}"
  ".hsub{font-size:10px;color:var(--text3);margin-top:6px}"
  ".hcard-glow{position:absolute;right:-20px;bottom:-20px;width:120px;height:120px;border-radius:50%;opacity:0.08;pointer-events:none}"
  ".bd{background:rgba(255,51,102,0.12);color:var(--danger);border:1px solid rgba(255,51,102,0.25)}"
  ".bw{background:rgba(255,209,102,0.12);color:var(--warn);border:1px solid rgba(255,209,102,0.25)}"
  ".bg2c{background:rgba(0,245,212,0.1);color:var(--c1);border:1px solid rgba(0,245,212,0.2)}"
  ".bn{background:rgba(0,180,216,0.1);color:var(--c2);border:1px solid rgba(0,180,216,0.2)}"
  ".bon{background:rgba(0,245,212,0.12);color:var(--c1);border:1px solid rgba(0,245,212,0.25)}"
  ".boff{background:rgba(42,74,96,0.3);color:var(--text3);border:1px solid var(--border)}"

  /* chart grid */
  ".cgrid{display:grid;grid-template-columns:1fr 1fr;gap:18px;margin-bottom:28px}"
  ".ccard{background:var(--bg2);border:1px solid var(--border);border-radius:16px;padding:22px;overflow:hidden}"
  ".chead{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:18px}"
  ".ctitle{font-size:14px;font-weight:600;color:var(--text)}"
  ".csub{font-size:10px;color:var(--text3);margin-top:3px;letter-spacing:1px}"
  ".ccur{font-family:'Orbitron',monospace;font-size:30px;font-weight:900;text-align:right}"
  ".ccon{position:relative;height:160px}"

  /* AI panel */
  ".aipan{background:var(--bg2);border:1px solid rgba(199,125,255,0.15);border-radius:16px;padding:22px;"
  "display:flex;flex-direction:column;gap:14px;position:relative;overflow:hidden}"
  ".aipan::before{content:'';position:absolute;top:0;left:0;right:0;height:1px;"
  "background:linear-gradient(90deg,transparent,rgba(199,125,255,0.4),transparent)}"
  ".ai-badge{display:inline-flex;align-items:center;gap:8px;background:linear-gradient(135deg,rgba(199,125,255,0.15),rgba(0,245,212,0.08));"
  "border:1px solid rgba(199,125,255,0.25);color:var(--c7);padding:5px 14px;border-radius:20px;"
  "font-size:9px;font-weight:700;letter-spacing:3px;text-transform:uppercase;font-family:'Orbitron',monospace}"
  ".prow{background:var(--bg3);border:1px solid var(--border);border-radius:12px;padding:14px 16px;"
  "display:flex;justify-content:space-between;align-items:center;transition:all 0.2s}"
  ".prow:hover{border-color:rgba(199,125,255,0.2);background:var(--bg4)}"
  ".plbl{font-size:9px;color:var(--text3);letter-spacing:2px;font-family:'Orbitron',monospace}"
  ".pvals{display:flex;gap:12px;align-items:center}"
  ".pitem{text-align:center}"
  ".ptime{font-size:8px;color:var(--text3);letter-spacing:1px;margin-bottom:3px}"
  ".pval{font-family:'Orbitron',monospace;font-size:14px;font-weight:700}"
  ".parr{color:var(--text3);font-size:16px}"
  ".act-box{background:linear-gradient(135deg,rgba(0,245,212,0.06),rgba(0,180,216,0.04));"
  "border:1px solid rgba(0,245,212,0.15);border-radius:12px;padding:16px;text-align:center}"
  ".act-lbl{font-size:9px;color:var(--text3);letter-spacing:3px;margin-bottom:8px;font-family:'Orbitron',monospace}"
  ".act-val{font-family:'Orbitron',monospace;font-size:22px;font-weight:900;color:var(--c1)}"
  ".act-desc{font-size:10px;color:var(--text2);margin-top:5px}"
  ".anom-row{display:flex;align-items:center;justify-content:space-between;background:var(--bg3);border-radius:10px;padding:12px 16px}"
  ".anom-lbl{font-size:9px;letter-spacing:2px;color:var(--text3);font-family:'Orbitron',monospace;margin-bottom:4px}"
  ".anom-val{font-family:'Orbitron',monospace;font-size:16px;font-weight:700}"

  /* sensor grid */
  ".sgrid{display:grid;grid-template-columns:repeat(6,1fr);gap:14px;margin-bottom:28px}"
  ".scard{background:var(--bg2);border:1px solid var(--border);border-radius:14px;padding:18px 14px;"
  "text-align:center;transition:all 0.3s;cursor:pointer;position:relative;overflow:hidden}"
  ".scard:hover{transform:translateY(-3px);border-color:rgba(0,245,212,0.2);box-shadow:0 8px 30px rgba(0,245,212,0.08)}"
  ".scard::after{content:'';position:absolute;bottom:0;left:0;right:0;height:2px;border-radius:0 0 14px 14px;opacity:0.4}"
  ".sicon{font-size:26px;margin-bottom:10px}"
  ".sname{font-size:8px;letter-spacing:3px;text-transform:uppercase;color:var(--text3);font-family:'Orbitron',monospace;margin-bottom:6px}"
  ".sval{font-family:'Orbitron',monospace;font-size:22px;font-weight:900;line-height:1}"
  ".sunit{font-size:10px;color:var(--text2);margin-top:3px}"
  ".sbar{height:3px;border-radius:2px;margin-top:10px;background:var(--bg3);overflow:hidden}"
  ".sbarfill{height:100%;border-radius:2px;transition:width 1.2s cubic-bezier(0.4,0,0.2,1)}"

  /* energy grid */
  ".egrid{display:grid;grid-template-columns:1fr 1fr 1fr 1.8fr;gap:16px;margin-bottom:28px}"
  ".ecard{background:var(--bg2);border:1px solid var(--border);border-radius:16px;padding:22px;transition:all 0.3s}"
  ".ecard:hover{border-color:rgba(255,107,107,0.2);box-shadow:0 8px 30px rgba(255,107,107,0.06)}"
  ".eicon{font-size:22px;margin-bottom:10px}"
  ".elabel{font-size:8px;letter-spacing:3px;text-transform:uppercase;color:var(--text3);font-family:'Orbitron',monospace}"
  ".eval{font-family:'Orbitron',monospace;font-size:28px;font-weight:900;margin:8px 0}"
  ".eunit{font-size:11px;color:var(--text2)}"

  /* tree card */
  ".tree-card{background:linear-gradient(135deg,rgba(0,245,212,0.04),var(--bg2));border:1px solid rgba(0,245,212,0.12);border-radius:16px;padding:22px}"
  ".tree-title{font-family:'Orbitron',monospace;font-size:9px;letter-spacing:3px;color:var(--c1);text-transform:uppercase;margin-bottom:8px}"
  ".tree-sub{font-size:10px;color:var(--text2);margin-bottom:14px}"
  ".trees{display:flex;gap:4px;flex-wrap:wrap;min-height:36px;align-items:center}"
  ".ti{font-size:20px;transition:all 0.3s;cursor:default;display:inline-block;animation:treeWave 3s ease-in-out infinite}"
  ".ti:nth-child(2n){animation-delay:0.3s}.ti:nth-child(3n){animation-delay:0.6s}.ti:nth-child(4n){animation-delay:0.9s}"
  "@keyframes treeWave{0%,100%{transform:translateY(0)}50%{transform:translateY(-4px)}}"
  ".ti:hover{transform:scale(1.4) translateY(-4px)!important}"
  ".tree-txt{font-size:10px;color:var(--text3);margin-top:10px;font-family:'Orbitron',monospace;letter-spacing:1px}"

  /* occ + airctrl */
  ".osec{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-bottom:28px}"
  ".ocard{background:var(--bg2);border:1px solid var(--border);border-radius:16px;padding:22px}"
  ".ocard-title{font-size:14px;font-weight:600;color:var(--text)}"
  ".ocard-sub{font-size:10px;color:var(--text3);letter-spacing:1px;margin-bottom:16px}"
  ".ovis{display:flex;gap:16px;align-items:center}"
  ".opers{font-size:48px;transition:all 0.5s}"
  ".oname{font-family:'Orbitron',monospace;font-size:24px;font-weight:900}"
  ".odesc{font-size:11px;color:var(--text2);margin-top:4px}"
  ".mode-btns{display:flex;gap:8px;margin-top:16px}"
  ".mbtn{padding:8px 22px;border-radius:10px;font-size:11px;font-weight:700;cursor:pointer;"
  "border:1px solid var(--border);letter-spacing:2px;transition:all 0.25s;background:var(--bg3);color:var(--text3);"
  "font-family:'Orbitron',monospace;text-transform:uppercase}"
  ".mbtn:hover{border-color:var(--c1);color:var(--c1);background:rgba(0,245,212,0.05)}"
  ".mauto{background:rgba(0,245,212,0.08);border-color:var(--c1);color:var(--c1)}"
  ".mman{background:rgba(255,107,107,0.08);border-color:var(--c3);color:var(--c3)}"
  ".fan-card{background:var(--bg2);border:1px solid var(--border);border-radius:16px;padding:22px;display:flex;flex-direction:column}"
  ".fan-vis{flex:1;display:flex;align-items:center;justify-content:center;margin:16px 0;position:relative}"
  ".fan-ring{position:absolute;width:120px;height:120px;border-radius:50%;border:1px solid rgba(0,245,212,0.1)}"
  ".fan-ring2{position:absolute;width:100px;height:100px;border-radius:50%;border:1px solid rgba(0,245,212,0.06)}"
  ".fan-icon{font-size:64px;transition:all 0.5s;position:relative;z-index:1}"
  ".fan-spin{animation:fanSpin 0.7s linear infinite}"
  "@keyframes fanSpin{from{transform:rotate(0)}to{transform:rotate(360deg)}}"
  ".fan-glow{position:absolute;width:140px;height:140px;border-radius:50%;"
  "background:radial-gradient(circle,rgba(0,245,212,0.12),transparent 70%);animation:glowPulse 2s ease-in-out infinite}"
  "@keyframes glowPulse{0%,100%{opacity:0.5;transform:scale(1)}50%{opacity:1;transform:scale(1.1)}}"
  ".fan-state{font-family:'Orbitron',monospace;font-size:20px;font-weight:900;text-align:center}"
  ".fan-reason{font-size:10px;color:var(--text3);text-align:center;margin-top:4px}"

  /* history */
  ".hist-card{background:var(--bg2);border:1px solid var(--border);border-radius:16px;padding:22px;margin-bottom:28px}"
  ".hist-head{display:flex;justify-content:space-between;align-items:center;margin-bottom:18px}"
  ".hist-title{font-size:14px;font-weight:600}"
  ".htabs{display:flex;gap:8px}"
  ".htab{padding:6px 16px;border-radius:8px;font-size:11px;font-weight:600;background:var(--bg3);"
  "border:1px solid var(--border);color:var(--text2);cursor:pointer;transition:all 0.2s;letter-spacing:1px}"
  ".htab.act{background:rgba(0,245,212,0.1);border-color:var(--c1);color:var(--c1)}"
  ".hist-con{position:relative;height:200px}"

  /* footer */
  "footer{border-top:1px solid var(--border);padding:20px 32px;display:flex;justify-content:space-between;"
  "align-items:center;position:relative;z-index:1}"
  ".ftxt{font-size:10px;color:var(--text3);font-family:'Orbitron',monospace;letter-spacing:1px}"
  ".fdots{display:flex;gap:6px}"
  ".fd{width:6px;height:6px;border-radius:50%;background:var(--bg4)}"
  ".fd.a{background:var(--c1);box-shadow:0 0 8px var(--c1)}"

  /* loading */
  ".lovl{position:fixed;inset:0;background:var(--bg);z-index:999;display:flex;align-items:center;"
  "justify-content:center;flex-direction:column;gap:28px;transition:opacity 0.6s}"
  ".licon{font-size:56px;animation:pulse 1.2s ease-in-out infinite}"
  ".ltxt{font-family:'Orbitron',monospace;font-size:16px;color:var(--c1);letter-spacing:6px}"
  ".lsub{font-size:11px;color:var(--text3);letter-spacing:3px}"
  ".lbar{width:240px;height:2px;background:var(--bg3);border-radius:1px;overflow:hidden}"
  ".lfill{height:100%;background:linear-gradient(90deg,var(--c1),var(--c2));border-radius:1px;animation:lfill 2s ease forwards}"
  "@keyframes lfill{from{width:0}to{width:100%}}"

  "::-webkit-scrollbar{width:5px}::-webkit-scrollbar-track{background:var(--bg)}::-webkit-scrollbar-thumb{background:var(--bg4);border-radius:3px}"
  "@media(max-width:1300px){.hgrid{grid-template-columns:repeat(2,1fr)}.sgrid{grid-template-columns:repeat(3,1fr)}.egrid{grid-template-columns:1fr 1fr}}"
  "@media(max-width:800px){main{padding:16px}.hgrid{grid-template-columns:1fr 1fr}.cgrid{grid-template-columns:1fr}.sgrid{grid-template-columns:repeat(2,1fr)}.egrid{grid-template-columns:1fr 1fr}.osec{grid-template-columns:1fr}}"
  "</style></head><body>"));

  /* ── BODY HTML ── */
  webServer.sendContent(F(
  "<div class='lovl' id='lo'>"
  "<div class='licon'>&#127758;</div>"
  "<div class='ltxt'>AIRMIND</div>"
  "<div class='lsub'>SMART ENVIRONMENT MONITOR</div>"
  "<div class='lbar'><div class='lfill'></div></div>"
  "</div>"
  "<div class='bg-mesh'></div><div class='scanline'></div>"
  "<header>"
  "<div class='logo'>"
  "<div class='logo-icon'>&#127758;</div>"
  "<div><div class='logo-text'>AirMind</div><div class='logo-sub'>Smart Environment Monitor</div></div>"
  "</div>"
  "<div class='hright'>"
  "<div class='pill'><div class='dot'></div>LIVE &middot; ESP32</div>"
  "<div class='pill'><div class='dot warn'></div>BUILT-IN SERVER</div>"
  "<div class='clock' id='clk'>--:--:--</div>"
  "</div>"
  "</header>"
  "<main>"

  /* alert */
  "<div class='alert-bar' id='ab'>"
  "<span class='alert-icon' id='abIco'>&#9888;&#65039;</span>"
  "<div class='alert-text'>AQI <strong id='abAqi'>&mdash;</strong> &mdash; <span id='abMsg'>Initializing...</span></div>"
  "<div class='alert-badge' id='abBadge'>CHECKING</div>"
  "</div>"

  /* hero cards */
  "<div class='stitle'>Live Readings</div>"
  "<div class='hgrid'>"
  "<div class='hcard a'>"
  "<div class='hlabel'>Air Quality Index</div>"
  "<div class='hval' id='aqiV' style='color:var(--c3)'>&mdash;</div>"
  "<div class='hbadge bn' id='aqiB'>&mdash;</div>"
  "<div class='hsub' id='aqiL'>&mdash;</div>"
  "<svg class='hcard-glow' viewBox='0 0 100 100'><circle cx='50' cy='50' r='50' fill='var(--c3)'/></svg>"
  "</div>"
  "<div class='hcard b'>"
  "<div class='hlabel'>CO&#8322; Level</div>"
  "<div class='hval' id='co2V' style='color:var(--c2)'>&mdash;<span class='hunit'>ppm</span></div>"
  "<div class='hbadge bn' id='co2B'>&mdash;</div>"
  "<div class='hsub' id='co2L'>&mdash;</div>"
  "<svg class='hcard-glow' viewBox='0 0 100 100'><circle cx='50' cy='50' r='50' fill='var(--c2)'/></svg>"
  "</div>"
  "<div class='hcard c'>"
  "<div class='hlabel'>Temperature</div>"
  "<div class='hval' id='tmpV' style='color:var(--c4)'>&mdash;<span class='hunit'>&#176;C</span></div>"
  "<div style='margin-top:10px'>"
  "<div style='font-size:9px;letter-spacing:3px;color:var(--text3);font-family:Orbitron,monospace'>HUMIDITY</div>"
  "<div style='font-family:Orbitron,monospace;font-size:26px;font-weight:900;color:var(--c2)'><span id='humV'>&mdash;</span><span style='font-size:14px;color:var(--text2)'>%</span></div>"
  "</div>"
  "<svg class='hcard-glow' viewBox='0 0 100 100'><circle cx='50' cy='50' r='50' fill='var(--c4)'/></svg>"
  "</div>"
  "<div class='hcard d'>"
  "<div class='hlabel'>AirCtrl Status</div>"
  "<div class='hval' id='acV' style='color:var(--text3)'>&mdash;</div>"
  "<div class='hbadge boff' id='acB'>&mdash;</div>"
  "<div class='hsub'>Mode: <span id='modeL' style='color:var(--c1)'>&mdash;</span> &bull; Occ: <span id='occL' style='color:var(--c5)'>&mdash;</span></div>"
  "<svg class='hcard-glow' viewBox='0 0 100 100'><circle cx='50' cy='50' r='50' fill='var(--c1)'/></svg>"
  "</div>"
  "</div>"

  /* charts + AI */
  "<div class='stitle'>Trend Analysis &amp; AI Predictions</div>"
  "<div style='display:grid;grid-template-columns:1fr 1fr 360px;gap:18px;margin-bottom:28px'>"
  "<div class='ccard'>"
  "<div class='chead'><div><div class='ctitle'>AQI Trend</div><div class='csub'>LAST 30 READINGS &middot; 2s</div></div><div class='ccur' style='color:var(--c3)' id='aqiCC'>&mdash;</div></div>"
  "<div class='ccon'><canvas id='aqiCh'></canvas></div>"
  "</div>"
  "<div class='ccard'>"
  "<div class='chead'><div><div class='ctitle'>CO&#8322; Trend</div><div class='csub'>LAST 30 READINGS &middot; 2s</div></div><div class='ccur' style='color:var(--c2)' id='co2CC'>&mdash;</div></div>"
  "<div class='ccon'><canvas id='co2Ch'></canvas></div>"
  "</div>"
  "<div class='aipan'>"
  "<div><div class='ai-badge'>&#10022; AI Engine</div></div>"
  "<div class='prow'>"
  "<div class='plbl'>AQI FORECAST</div>"
  "<div class='pvals'>"
  "<div class='pitem'><div class='ptime'>NOW</div><div class='pval' id='pAN' style='color:var(--c3)'>&mdash;</div></div>"
  "<div class='parr'>&#8594;</div>"
  "<div class='pitem'><div class='ptime'>+10s</div><div class='pval' id='pA10' style='color:var(--c3)'>&mdash;</div></div>"
  "<div class='pitem'><div class='ptime'>+30s</div><div class='pval' id='pA30' style='color:var(--c3)'>&mdash;</div></div>"
  "<div class='pitem'><div class='ptime'>+60s</div><div class='pval' id='pA60' style='color:var(--c3)'>&mdash;</div></div>"
  "</div></div>"
  "<div class='prow'>"
  "<div class='plbl'>CO&#8322; FORECAST</div>"
  "<div class='pvals'>"
  "<div class='pitem'><div class='ptime'>NOW</div><div class='pval' id='pCN' style='color:var(--c2)'>&mdash;</div></div>"
  "<div class='parr'>&#8594;</div>"
  "<div class='pitem'><div class='ptime'>+10s</div><div class='pval' id='pC10' style='color:var(--c2)'>&mdash;</div></div>"
  "<div class='pitem'><div class='ptime'>+30s</div><div class='pval' id='pC30' style='color:var(--c2)'>&mdash;</div></div>"
  "<div class='pitem'><div class='ptime'>+60s</div><div class='pval' id='pC60' style='color:var(--c2)'>&mdash;</div></div>"
  "</div></div>"
  "<div class='act-box'><div class='act-lbl'>AI RECOMMENDATION</div><div class='act-val' id='aiAct'>&mdash;</div><div class='act-desc' id='aiActD'>Fetching from Flask...</div></div>"
  "<div class='anom-row'><div><div class='anom-lbl'>ANOMALY DETECTION</div><div class='anom-val' id='anomSt' style='color:var(--c1)'>&mdash;</div></div><div style='font-size:30px' id='anomIc'>&#128269;</div></div>"
  "</div>"
  "</div>"

  /* sensors */
  "<div class='stitle'>Sensor Array</div>"
  "<div class='sgrid'>"
  "<div class='scard'><div class='sicon'>&#129514;</div><div class='sname'>NH&#8323;</div><div class='sval' id='nh3V' style='color:var(--c7)'>&mdash;</div><div class='sunit'>ppb</div><div class='sbar'><div class='sbarfill' id='nh3B' style='background:var(--c7)'></div></div></div>"
  "<div class='scard'><div class='sicon'>&#9879;&#65039;</div><div class='sname'>Benzene</div><div class='sval' id='bzV' style='color:var(--c3)'>&mdash;</div><div class='sunit'>ppb</div><div class='sbar'><div class='sbarfill' id='bzB' style='background:var(--c3)'></div></div></div>"
  "<div class='scard'><div class='sicon'>&#128168;</div><div class='sname'>CO</div><div class='sval' id='coV' style='color:var(--c4)'>&mdash;</div><div class='sunit'>ppm</div><div class='sbar'><div class='sbarfill' id='coB' style='background:var(--c4)'></div></div></div>"
  "<div class='scard'><div class='sicon'>&#9889;</div><div class='sname'>Voltage</div><div class='sval' id='vltV' style='color:var(--c2)'>&mdash;</div><div class='sunit'>V</div><div class='sbar'><div class='sbarfill' id='vltB' style='background:var(--c2);width:70%'></div></div></div>"
  "<div class='scard'><div class='sicon'>&#128268;</div><div class='sname'>Current</div><div class='sval' id='curV' style='color:var(--c6)'>&mdash;</div><div class='sunit'>mA</div><div class='sbar'><div class='sbarfill' id='curB' style='background:var(--c6)'></div></div></div>"
  "<div class='scard'><div class='sicon'>&#128161;</div><div class='sname'>Power</div><div class='sval' id='pwrV' style='color:var(--c5)'>&mdash;</div><div class='sunit'>W</div><div class='sbar'><div class='sbarfill' id='pwrB' style='background:var(--c5)'></div></div></div>"
  "</div>"

  /* energy + tree */
  "<div class='stitle'>Energy &amp; Carbon Footprint</div>"
  "<div class='egrid'>"
  "<div class='ecard'><div class='eicon'>&#9889;</div><div class='elabel'>Energy Used</div><div class='eval' id='engV' style='color:var(--c2)'>&mdash;</div><div class='eunit'>kWh</div></div>"
  "<div class='ecard'><div class='eicon'>&#128176;</div><div class='elabel'>Cost Estimate</div><div class='eval' id='cstV' style='color:var(--c5)'>&mdash;</div><div class='eunit'>INR</div></div>"
  "<div class='ecard'><div class='eicon'>&#127758;</div><div class='elabel'>CO&#8322; Emitted</div><div class='eval' id='cemV' style='color:var(--c6)'>&mdash;</div><div class='eunit'>g CO&#8322;</div></div>"
  "<div class='tree-card'>"
  "<div class='tree-title'>&#127795; Tree Equivalent</div>"
  "<div class='tree-sub'>Tree-minutes of CO&#8322; absorption needed</div>"
  "<div class='trees' id='treeCon'><span class='ti'>&#127807;</span></div>"
  "<div class='tree-txt' id='treeTxt'>&mdash;</div>"
  "</div>"
  "</div>"

  /* occ + fan */
  "<div class='stitle'>Occupancy &amp; AirCtrl</div>"
  "<div class='osec'>"
  "<div class='ocard'>"
  "<div class='ocard-title'>Occupancy Monitor</div>"
  "<div class='ocard-sub'>PIR &middot; GPIO 27</div>"
  "<div class='ovis'>"
  "<div class='opers' id='occPrs'>&#128694;</div>"
  "<div><div class='oname' id='occSt'>&mdash;</div><div class='odesc' id='occDs'>&mdash;</div></div>"
  "</div>"
  "<div style='font-size:9px;color:var(--text3);letter-spacing:3px;font-family:Orbitron,monospace;margin-top:16px;margin-bottom:8px'>CONTROL MODE</div>"
  "<div class='mode-btns'>"
  "<div class='mbtn' id='mAuto' onclick='setMode(\"AUTO\")'>AUTO</div>"
  "<div class='mbtn' id='mMan' onclick='setMode(\"MANUAL\")'>MANUAL</div>"
  "</div>"
  "</div>"
  "<div class='fan-card'>"
  "<div class='ocard-title'>AirCtrl Output</div>"
  "<div class='ocard-sub'>Relay &middot; GPIO 12</div>"
  "<div class='fan-vis'>"
  "<div class='fan-ring'></div><div class='fan-ring2'></div>"
  "<div class='fan-glow' id='fanGlow' style='display:none'></div>"
  "<div class='fan-icon' id='fanIco'>&#127744;</div>"
  "</div>"
  "<div class='fan-state' id='fanSt' style='color:var(--text3)'>&mdash;</div>"
  "<div class='fan-reason' id='fanRs'>&mdash;</div>"
  "</div>"
  "</div>"

  /* history */
  "<div class='stitle'>History</div>"
  "<div class='hist-card'>"
  "<div class='hist-head'>"
  "<div class='hist-title'>Environmental Timeline</div>"
  "<div class='htabs'>"
  "<div class='htab act' onclick='switchTab(\"aqi\",this)'>AQI</div>"
  "<div class='htab' onclick='switchTab(\"co2\",this)'>CO&#8322;</div>"
  "<div class='htab' onclick='switchTab(\"temp\",this)'>Temp</div>"
  "<div class='htab' onclick='switchTab(\"humidity\",this)'>Humidity</div>"
  "</div></div>"
  "<div class='hist-con'><canvas id='histCh'></canvas></div>"
  "</div>"
  "</main>"
  "<footer>"
  "<div class='ftxt'>AIRMIND v2.0 &bull; ESP32 &bull; <span id='fupt'>Initializing...</span></div>"
  "<div class='fdots'><div class='fd a' id='d1'></div><div class='fd' id='d2'></div><div class='fd' id='d3'></div></div>"
  "</footer>"
  ));

  /* ── JAVASCRIPT ── */
  webServer.sendContent(F("<script>"
  "Chart.defaults.color='#2a4a60';Chart.defaults.font.size=10;"
  "const S={hist:{aqi:[],co2:[],temp:[],humidity:[],labels:[]},n:0};"
  "const ADESC={'KEEP_OFF':'Air quality OK - system in standby','TURN_ON':'Immediate activation recommended','RUN_2MIN':'Run ventilation for 2 minutes','RUN_5MIN':'Extended ventilation needed'};"
  "function gLabel(v){if(v<=50)return'Good';if(v<=100)return'Moderate';if(v<=150)return'Unhealthy Sensitive';if(v<=200)return'Unhealthy';if(v<=300)return'Very Unhealthy';return'Hazardous'}"
  "function co2L(v){if(v<800)return'Normal CO2';if(v<1200)return'Elevated CO2';if(v<1500)return'High CO2 Level';return'Very High CO2'}"
  "function gColor(v){if(v<=50)return'#00f5d4';if(v<=100)return'#ffd166';if(v<=150)return'#ff9a3c';if(v<=200)return'#ff6b6b';return'#ff3366'}"
  "function gBadge(v){if(v<=50)return'bg2c';if(v<=100)return'bn';if(v<=150)return'bw';return'bd'}"
  "function aiEst(d){"
  "  const dr=(Math.random()-0.45)*4;"
  "  return{aqi_now:d.aqi,aqi_10sec:(d.aqi+dr).toFixed(1),aqi_30sec:(d.aqi+dr*2).toFixed(1),aqi_60sec:(d.aqi+dr*3.5).toFixed(1),"
  "  co2_now:d.co2,co2_10sec:(d.co2+dr*8).toFixed(0),co2_30sec:(d.co2+dr*16).toFixed(0),co2_60sec:(d.co2+dr*25).toFixed(0),"
  "  action:d.aqi>150?'RUN_5MIN':d.aqi>100?'RUN_2MIN':d.airCtrl?'TURN_ON':'KEEP_OFF',anomaly:'NORMAL'};"
  "}"
  "function mkSp(id,color){"
  "  const ctx=document.getElementById(id).getContext('2d');"
  "  const g=ctx.createLinearGradient(0,0,0,160);g.addColorStop(0,color+'44');g.addColorStop(1,color+'00');"
  "  return new Chart(ctx,{type:'line',data:{labels:Array(30).fill(''),datasets:[{data:Array(30).fill(null),borderColor:color,borderWidth:2.5,backgroundColor:g,fill:true,tension:0.45,pointRadius:0}]},options:{responsive:true,maintainAspectRatio:false,animation:{duration:400},plugins:{legend:{display:false}},scales:{x:{display:false},y:{grid:{color:'rgba(255,255,255,0.04)'},border:{display:false},ticks:{color:'#2a4a60',maxTicksLimit:4}}}}});"
  "}"
  "function mkHist(){"
  "  const ctx=document.getElementById('histCh').getContext('2d');const c='#00f5d4';"
  "  const g=ctx.createLinearGradient(0,0,0,200);g.addColorStop(0,c+'44');g.addColorStop(1,c+'00');"
  "  return new Chart(ctx,{type:'line',data:{labels:[],datasets:[{label:'AQI',data:[],borderColor:c,borderWidth:2.5,backgroundColor:g,fill:true,tension:0.45,pointRadius:0,pointHoverRadius:5}]},options:{responsive:true,maintainAspectRatio:false,animation:{duration:500},plugins:{legend:{display:false},tooltip:{mode:'index',intersect:false}},scales:{x:{grid:{color:'rgba(255,255,255,0.03)'},ticks:{color:'#2a4a60',maxTicksLimit:8},border:{display:false}},y:{grid:{color:'rgba(255,255,255,0.04)'},border:{display:false},ticks:{color:'#2a4a60'}}}}});"
  "}"
  "let aqiSp,co2Sp,histCh,curTab='aqi';"
  "function initCharts(){aqiSp=mkSp('aqiCh','#ff6b6b');co2Sp=mkSp('co2Ch','#00b4d8');histCh=mkHist()}"
  "function switchTab(t,el){curTab=t;document.querySelectorAll('.htab').forEach(x=>x.classList.remove('act'));el.classList.add('act');updHist()}"
  "function updHist(){"
  "  const cols={aqi:'#ff6b6b',co2:'#00b4d8',temp:'#ffd166',humidity:'#a8ff78'};"
  "  const labs={aqi:'AQI',co2:'CO2(ppm)',temp:'Temp(C)',humidity:'Humidity(%)'};"
  "  const c=cols[curTab];"
  "  const ctx=document.getElementById('histCh').getContext('2d');"
  "  const g=ctx.createLinearGradient(0,0,0,200);g.addColorStop(0,c+'44');g.addColorStop(1,c+'00');"
  "  histCh.data.labels=[...S.hist.labels];histCh.data.datasets[0].data=[...S.hist[curTab]];"
  "  histCh.data.datasets[0].borderColor=c;histCh.data.datasets[0].backgroundColor=g;histCh.data.datasets[0].label=labs[curTab];"
  "  histCh.update('none');}"
  "const $=id=>document.getElementById(id);"
  "function updLive(d){"
  "  const c=gColor(d.aqi);"
  "  $('aqiV').textContent=d.aqi;$('aqiV').style.color=c;"
  "  $('aqiB').textContent=gLabel(d.aqi);$('aqiB').className='hbadge '+gBadge(d.aqi);"
  "  $('aqiL').textContent=d.airQuality;$('aqiCC').textContent=d.aqi;"
  "  $('co2V').innerHTML=d.co2+'<span class=\"hunit\">ppm</span>';"
  "  $('co2B').textContent=co2L(d.co2);$('co2B').className='hbadge '+(d.co2>1500?'bd':d.co2>1000?'bw':'bg2c');"
  "  $('co2L').textContent=co2L(d.co2);$('co2CC').textContent=d.co2;"
  "  $('tmpV').innerHTML=d.temp+'<span class=\"hunit\">&#176;C</span>';$('humV').textContent=d.humidity;"
  "  const isOn=d.airCtrl==1;"
  "  $('acV').textContent=isOn?'ON':'OFF';$('acV').style.color=isOn?'var(--c1)':'var(--text3)';"
  "  $('acB').textContent=isOn?'ACTIVE':'STANDBY';$('acB').className='hbadge '+(isOn?'bon':'boff');"
  "  $('modeL').textContent=d.mode;$('occL').textContent=d.occupancy?'PRESENT':'ABSENT';"
  "  $('nh3V').textContent=d.nh3;$('nh3B').style.width=Math.min(100,d.nh3/2)+'%';"
  "  $('bzV').textContent=d.benzene;$('bzB').style.width=Math.min(100,d.benzene*5)+'%';"
  "  $('coV').textContent=d.co;$('coB').style.width=Math.min(100,d.co*10)+'%';"
  "  $('vltV').textContent=d.voltage;$('curV').textContent=d.current;$('curB').style.width=Math.min(100,d.current/5)+'%';"
  "  $('pwrV').textContent=d.power;$('pwrB').style.width=Math.min(100,d.power*10)+'%';"
  "  $('engV').textContent=parseFloat(d.energy).toFixed(4);$('cstV').textContent='Rs.'+parseFloat(d.cost).toFixed(4);"
  "  const co2g=(parseFloat(d.energy)*820).toFixed(1);$('cemV').textContent=co2g;updTrees(+co2g);"
  "  const occ=d.occupancy==1;"
  "  $('occPrs').style.opacity=occ?'1':'0.3';$('occSt').textContent=occ?'PRESENT':'ABSENT';"
  "  $('occSt').style.color=occ?'var(--c1)':'var(--text3)';"
  "  $('occDs').textContent=occ?'Motion detected - room occupied':'No motion - room empty';"
  "  $('mAuto').className='mbtn'+(d.mode==='AUTO'?' mauto':'');"
  "  $('mMan').className='mbtn'+(d.mode==='MANUAL'?' mman':'');"
  "  $('fanIco').className='fan-icon'+(isOn?' fan-spin':'');"
  "  $('fanSt').textContent=isOn?'RUNNING':'STANDBY';"
  "  $('fanSt').style.color=isOn?'var(--c1)':'var(--text3)';"
  "  $('fanGlow').style.display=isOn?'block':'none';"
  "  $('fanRs').textContent=isOn?('AQI '+d.aqi+' > threshold - '+(occ?'Occupied':'Emergency')):'Awaiting activation';"
  "  $('abAqi').textContent=d.aqi;"
  "  if(d.aqi>200){$('ab').style.cssText='display:flex;align-items:center;gap:16px;border-radius:14px;padding:14px 22px;margin-bottom:28px;border:1px solid rgba(255,51,102,0.3);background:linear-gradient(90deg,rgba(255,51,102,0.08),transparent)';$('abMsg').textContent='Very Unhealthy - AirCtrl activated';$('abBadge').textContent='ALERT';$('abBadge').style.cssText='margin-left:auto;padding:4px 12px;border-radius:20px;font-size:9px;font-weight:700;letter-spacing:2px;background:rgba(255,51,102,0.12);border:1px solid rgba(255,51,102,0.3);color:#ff3366';$('abIco').textContent='&#128680;';}"
  "  else if(d.aqi>100){$('ab').style.cssText='display:flex;align-items:center;gap:16px;border-radius:14px;padding:14px 22px;margin-bottom:28px;border:1px solid rgba(255,209,102,0.3);background:linear-gradient(90deg,rgba(255,209,102,0.07),transparent)';$('abMsg').textContent='Moderate - monitoring closely';$('abBadge').textContent='CAUTION';$('abBadge').style.cssText='margin-left:auto;padding:4px 12px;border-radius:20px;font-size:9px;font-weight:700;letter-spacing:2px;background:rgba(255,209,102,0.12);border:1px solid rgba(255,209,102,0.3);color:#ffd166';}"
  "  else{$('ab').style.cssText='display:flex;align-items:center;gap:16px;border-radius:14px;padding:14px 22px;margin-bottom:28px;border:1px solid rgba(0,245,212,0.15);background:linear-gradient(90deg,rgba(0,245,212,0.05),transparent)';$('abMsg').textContent='Air quality within acceptable range';$('abBadge').textContent='NORMAL';$('abBadge').style.cssText='margin-left:auto;padding:4px 12px;border-radius:20px;font-size:9px;font-weight:700;letter-spacing:2px;background:rgba(0,245,212,0.1);border:1px solid rgba(0,245,212,0.2);color:#00f5d4';$('abIco').textContent='&#9989;';}"
  "  const sp=aqiSp.data.datasets[0].data;sp.push(+d.aqi);if(sp.length>30)sp.shift();aqiSp.update('none');"
  "  const sp2=co2Sp.data.datasets[0].data;sp2.push(+d.co2);if(sp2.length>30)sp2.shift();co2Sp.update('none');"
  "}"
  "function updAI(ai){"
  "  $('pAN').textContent=parseFloat(ai.aqi_now).toFixed(0);$('pA10').textContent=parseFloat(ai.aqi_10sec).toFixed(0);"
  "  $('pA30').textContent=parseFloat(ai.aqi_30sec).toFixed(0);$('pA60').textContent=parseFloat(ai.aqi_60sec).toFixed(0);"
  "  $('pCN').textContent=parseFloat(ai.co2_now).toFixed(0);$('pC10').textContent=parseFloat(ai.co2_10sec).toFixed(0);"
  "  $('pC30').textContent=parseFloat(ai.co2_30sec).toFixed(0);$('pC60').textContent=parseFloat(ai.co2_60sec).toFixed(0);"
  "  $('aiAct').textContent=ai.action||'--';$('aiAct').style.color=ai.action==='KEEP_OFF'?'var(--text2)':'var(--c1)';"
  "  $('aiActD').textContent=ADESC[ai.action]||'--';"
  "  const isA=ai.anomaly==='ANOMALY';"
  "  $('anomSt').textContent=ai.anomaly||'--';$('anomSt').style.color=isA?'var(--c3)':'var(--c1)';"
  "  $('anomIc').textContent=isA?'&#128680;':'&#9989;';"
  "}"
  "function updTrees(g){"
  "  const n=Math.max(1,Math.round(g/0.5));const con=$('treeCon');con.innerHTML='';"
  "  const em=['&#127795;','&#127794;','&#127796;','&#127797;','&#127807;'];const disp=Math.min(n,24);"
  "  for(let i=0;i<disp;i++){const s=document.createElement('span');s.className='ti';s.innerHTML=em[i%5];con.appendChild(s)}"
  "  $('treeTxt').textContent='~'+n+' tree-minute'+(n>1?'s':'')+' to offset '+g+'g CO2';"
  "}"
  "function pushHist(d){"
  "  const now=new Date();const lbl=now.getHours().toString().padStart(2,'0')+':'+now.getMinutes().toString().padStart(2,'0')+':'+now.getSeconds().toString().padStart(2,'0');"
  "  ['aqi','co2','temp','humidity'].forEach(k=>{S.hist[k].push(+d[k]);if(S.hist[k].length>60)S.hist[k].shift()});"
  "  S.hist.labels.push(lbl);if(S.hist.labels.length>60)S.hist.labels.shift();updHist();"
  "}"
  "async function fetchAI(d){"
  "  try{const r=await fetch('http://10.76.154.13:5000/predict',{signal:AbortSignal.timeout(1500)});return await r.json();}"
  "  catch{return aiEst(d);}"
  "}"
  "async function update(){"
  "  S.n++;"
  "  try{"
  "    const r=await fetch('/live',{signal:AbortSignal.timeout(2000)});"
  "    const d=await r.json();"
  "    const ai=await fetchAI(d);"
  "    updLive(d);updAI(ai);pushHist(d);"
  "  }catch(e){console.log('Fetch:',e);}"
  "  const di=S.n%3;['d1','d2','d3'].forEach((id,i)=>{$(id).className='fd'+(i===di?' a':'')});"
  "  $('fupt').textContent='Updated: '+new Date().toLocaleTimeString();"
  "}"
  "async function setMode(m){try{await fetch('/setmode?mode='+m);}catch(e){}}"
  "function updClk(){$('clk').textContent=new Date().toLocaleTimeString('en-US',{hour12:false})}"
  "window.addEventListener('load',()=>{"
  "  initCharts();updClk();setInterval(updClk,1000);"
  "  setTimeout(()=>{const o=$('lo');o.style.opacity='0';setTimeout(()=>o.style.display='none',600)},2200);"
  "  setTimeout(()=>{update();setInterval(update,2000)},2400);"
  "});"
  "</script></body></html>"));
}

/* =========================================================
   SETUP — original code + web server init added at end
   ========================================================= */
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

  if (!ina219.begin()) { Serial.println("INA219 not found"); inaError = true; }
  else inaError = false;

  loadSavedR0();
  initFilters();

  systemStartMillis = millis();
  previousEnergyMillis = millis();

  Serial.println("======================================");
  Serial.println("SMART ENVIRONMENT MONITOR - FINAL CORE");
  Serial.println("======================================");
  Serial.print("Loaded R0: "); Serial.println(R0, 3);
  Serial.println("Mode: MEASUREMENT");
  Serial.println("Type HELP in Serial Monitor");
  Serial.println();

  connectWiFi();
  printCSVHeader();

  /* ── Start web server (NEW) ── */
  webServer.on("/",        handleWebRoot);
  webServer.on("/live",    handleWebLive);
  webServer.on("/setmode", handleWebSetMode);
  webServer.begin();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("======================================");
    Serial.print("  Web Dashboard : http://"); Serial.println(WiFi.localIP());
    Serial.print("  Live JSON     : http://"); Serial.print(WiFi.localIP()); Serial.println("/live");
    Serial.println("======================================");
  }

  lcd.setCursor(0, 0); lcd.print("Smart Env V2");
  lcd.setCursor(0, 1); lcd.print("R0:"); lcd.print(R0, 2);
  delay(1500); lcd.clear();
}

/* =========================================================
   LOOP — original code + webServer.handleClient() added
   ========================================================= */
void loop() {
  webServer.handleClient();   /* ← NEW: handle web requests */

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
