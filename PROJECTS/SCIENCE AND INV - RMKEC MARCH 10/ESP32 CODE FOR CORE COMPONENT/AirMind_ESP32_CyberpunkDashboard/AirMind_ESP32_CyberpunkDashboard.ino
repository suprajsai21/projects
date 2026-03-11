/*
 ╔══════════════════════════════════════════════════════════╗
 ║     AIRMIND — ESP32 FULL CYBERPUNK DASHBOARD SERVER      ║
 ║  Replaces simple HTML with full dashboard.html look      ║
 ║  API endpoints: /api/data  /api/mode                     ║
 ╚══════════════════════════════════════════════════════════╝
*/

#include <math.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

/* ── CHANGE THESE ── */
const char* ssid     = "ESP1";
const char* password = "12345678";

/* ── PINS ── */
const int MQ135_PIN = 34;
#define PIR_PIN    27
#define FAN_PIN    12
#define BUZZER_PIN 14
#define RED_LED    25
#define GREEN_LED  26
#define BLUE_LED   33
#define DHTPIN     4
#define DHTTYPE    DHT11

/* ── OBJECTS ── */
WebServer         server(80);
DHT               dht(DHTPIN, DHTTYPE);
Adafruit_INA219   ina219;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences       preferences;

/* ── MQ135 ── */
float R0=7.20;
const float RL=10.0,VC=3.3,CO2_A=116.6020682,CO2_B=-2.769034857;

/* ── ENERGY ── */
const float tariff=8.0;
float energy_Wh=0.0;
unsigned long prevEnergyMs=0;

/* ── TIMING ── */
unsigned long lastSensorMs=0,lastLCDMs=0,lastBuzzMs=0,startMs=0;

/* ── MODE ── */
enum Mode{AUTO_MODE,MANUAL_MODE};
Mode controlMode=AUTO_MODE;
unsigned long lastMotionMs=0;
const unsigned long OCC_DELAY=120000;

/* ── SMOOTHING ── */
const int FS=10;
int adcBuf[FS]={0};float tmpBuf[FS]={0},humBuf[FS]={0},curBuf[FS]={0},pwrBuf[FS]={0};
bool filterFull=false;int filterIdx=0;

/* ── LIVE DATA ── */
int    AQI=0;String airQuality="--";float co2ppm=400;String co2Status="--";
float  nh3=0,benzene=0,co=0,temp=0,hum=0,volt=0,curr=0,pwr=0,eng_kwh=0,cst=0;
int    occ=0,fanState=0;
bool   dhtErr=false,inaErr=false,mqErr=false,buzzState=false;
int    buzzPat=500,lcdPage=0;
unsigned long readCount=0;

/* ── HELPERS ── */
float getRs(int v){float vout=v*(3.3/4095.0);if(vout<0.01)vout=0.01;return((VC-vout)/vout)*RL;}
float getCO2(float rs){return CO2_A*pow(rs/R0,CO2_B);}
void  loadR0(){preferences.begin("envmon",true);if(preferences.isKey("R0"))R0=preferences.getFloat("R0",7.20);preferences.end();}
int   vc(){return filterFull?FS:filterIdx;}
float avgI(int*a){int n=vc();if(!n)return 0;long s=0;for(int i=0;i<n;i++)s+=a[i];return(float)s/n;}
float avgF(float*a){int n=vc();if(!n)return 0;float s=0;for(int i=0;i<n;i++)s+=a[i];return s/n;}
String modeStr(){return controlMode==AUTO_MODE?"AUTO":"MANUAL";}

void setAQ(int a){
  if(a<=25){airQuality="Excellent";nh3=1;benzene=0.3;co=0.05;}
  else if(a<=50){airQuality="Good";nh3=2;benzene=0.5;co=0.10;}
  else if(a<=75){airQuality="Fair";nh3=5;benzene=1.0;co=0.20;}
  else if(a<=100){airQuality="Moderate";nh3=10;benzene=2.0;co=0.30;}
  else if(a<=125){airQuality="Slightly Polluted";nh3=18;benzene=3.0;co=0.50;}
  else if(a<=150){airQuality="Unhealthy Sensitive";nh3=30;benzene=4.0;co=0.80;}
  else if(a<=175){airQuality="Mildly Unhealthy";nh3=45;benzene=5.5;co=1.10;}
  else if(a<=200){airQuality="Unhealthy";nh3=60;benzene=7.0;co=1.50;}
  else if(a<=225){airQuality="Very Unhealthy";nh3=80;benzene=8.5;co=2.00;}
  else if(a<=250){airQuality="Heavily Polluted";nh3=100;benzene=10.0;co=2.50;}
  else{airQuality="Hazardous";nh3=140;benzene=14.0;co=3.50;}
}
void setCO2S(float v){
  if(v<=450)co2Status="Fresh Outdoor Air";
  else if(v<=600)co2Status="Very Good Indoor";
  else if(v<=800)co2Status="Normal Indoor Air";
  else if(v<=1000)co2Status="Acceptable";
  else if(v<=1200)co2Status="Poor Ventilation";
  else if(v<=2000)co2Status="High CO2 Level";
  else co2Status="Very High CO2";
}

void readSensors(){
  int raw=analogRead(MQ135_PIN);mqErr=(raw<0||raw>4095);
  float rt=dht.readTemperature(),rh=dht.readHumidity();
  if(isnan(rt)||isnan(rh)){dhtErr=true;rt=temp;rh=hum;}else dhtErr=false;
  float rv=ina219.getBusVoltage_V(),rc=ina219.getCurrent_mA(),rp=ina219.getPower_mW()/1000.0;
  if(isnan(rv)||isnan(rc)||isnan(rp)){inaErr=true;rv=volt;rc=curr;rp=pwr;}else inaErr=false;
  adcBuf[filterIdx]=raw;tmpBuf[filterIdx]=rt;humBuf[filterIdx]=rh;curBuf[filterIdx]=rc;pwrBuf[filterIdx]=rp;
  filterIdx++;if(filterIdx>=FS){filterIdx=0;filterFull=true;}
  int sadc=(int)avgI(adcBuf);temp=avgF(tmpBuf);hum=avgF(humBuf);curr=avgF(curBuf);pwr=avgF(pwrBuf);volt=rv;
  co2ppm=getCO2(getRs(sadc));if(co2ppm<350)co2ppm=350;if(co2ppm>5000)co2ppm=5000;
  AQI=map(sadc,0,4095,0,500);
  setAQ(AQI);setCO2S(co2ppm);
  unsigned long now=millis();
  float hrs=(now-prevEnergyMs)/3600000.0;prevEnergyMs=now;
  energy_Wh+=pwr*hrs;eng_kwh=energy_Wh/1000.0;cst=eng_kwh*tariff;readCount++;
}
void updateOcc(){if(controlMode==MANUAL_MODE){occ=1;return;}if(digitalRead(PIR_PIN)==HIGH)lastMotionMs=millis();occ=(millis()-lastMotionMs<OCC_DELAY)?1:0;}
void updateFan(){if((occ==1&&AQI>100)||(occ==0&&AQI>200))digitalWrite(FAN_PIN,HIGH);else digitalWrite(FAN_PIN,LOW);fanState=digitalRead(FAN_PIN);}
void updateLEDs(){if(AQI<=50){digitalWrite(GREEN_LED,HIGH);digitalWrite(RED_LED,LOW);digitalWrite(BLUE_LED,LOW);}else if(AQI<=100){digitalWrite(GREEN_LED,LOW);digitalWrite(RED_LED,LOW);digitalWrite(BLUE_LED,HIGH);}else{digitalWrite(GREEN_LED,LOW);digitalWrite(BLUE_LED,LOW);digitalWrite(RED_LED,HIGH);}}
void handleBuzz(){if(AQI<=50)buzzPat=50;else if(AQI<=100)buzzPat=150;else if(AQI<=200)buzzPat=300;else buzzPat=500;unsigned long now=millis();if(!buzzState){if(now-lastBuzzMs>=2000){digitalWrite(BUZZER_PIN,HIGH);buzzState=true;lastBuzzMs=now;}}else{if(now-lastBuzzMs>=(unsigned long)buzzPat){digitalWrite(BUZZER_PIN,LOW);buzzState=false;lastBuzzMs=now;}}}
void updateLCD(){lcd.clear();if(lcdPage==0){lcd.setCursor(0,0);lcd.print("AQI:");lcd.print(AQI);lcd.setCursor(8,0);lcd.print("T:");lcd.print(temp,0);lcd.print("C");lcd.setCursor(0,1);lcd.print("CO2:");lcd.print((int)co2ppm);lcd.setCursor(10,1);lcd.print(occ?"OCC":"---");}else if(lcdPage==1){lcd.setCursor(0,0);lcd.print("I:");lcd.print(curr,0);lcd.print("mA");lcd.setCursor(0,1);lcd.print("P:");lcd.print(pwr,1);lcd.print("W");}else{lcd.setCursor(0,0);lcd.print("AirMind Online");lcd.setCursor(0,1);lcd.print(WiFi.localIP().toString());}lcdPage++;if(lcdPage>2)lcdPage=0;}

/* ══════════════════════════════
   API: /api/data  (JSON)
   ══════════════════════════════ */
void handleData(){
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.sendHeader("Cache-Control","no-cache");
  StaticJsonDocument<512> doc;
  doc["aqi"]=AQI;doc["airQuality"]=airQuality;
  doc["co2"]=(int)co2ppm;doc["co2Status"]=co2Status;
  doc["temp"]=round(temp*10)/10.0;doc["humidity"]=round(hum*10)/10.0;
  doc["nh3"]=round(nh3*10)/10.0;doc["benzene"]=round(benzene*10)/10.0;doc["co"]=round(co*100)/100.0;
  doc["voltage"]=round(volt*100)/100.0;doc["current"]=round(curr*10)/10.0;doc["power"]=round(pwr*1000)/1000.0;
  doc["energy"]=eng_kwh;doc["cost"]=cst;
  doc["occupancy"]=occ;doc["airCtrl"]=fanState;doc["mode"]=modeStr();
  doc["uptime"]=(millis()-startMs)/1000;doc["readings"]=readCount;
  String out;serializeJson(doc,out);
  server.send(200,"application/json",out);
}

/* ══════════════════════════════
   API: /api/mode
   ══════════════════════════════ */
void handleMode(){
  server.sendHeader("Access-Control-Allow-Origin","*");
  if(server.hasArg("set")){
    String m=server.arg("set");
    if(m=="AUTO"){controlMode=AUTO_MODE;server.send(200,"application/json","{\"ok\":true,\"mode\":\"AUTO\"}");}
    else if(m=="MANUAL"){controlMode=MANUAL_MODE;server.send(200,"application/json","{\"ok\":true,\"mode\":\"MANUAL\"}");}
    else server.send(400,"application/json","{\"error\":\"invalid\"}");
  }else server.send(400,"application/json","{\"error\":\"missing set param\"}");
}

/* ══════════════════════════════════════════════════════
   ROOT: /  — FULL CYBERPUNK DASHBOARD (matches dashboard.html)
   ══════════════════════════════════════════════════════ */
void handleRoot(){
  server.sendHeader("Cache-Control","no-cache");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"text/html","");

  /* ── CSS ── */
  server.sendContent(F("<!DOCTYPE html><html lang='en'><head>"
  "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>"
  "<title>AirMind &mdash; Smart Environment Control</title>"
  "<script src='https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.0/chart.umd.min.js'></script>"
  "<style>"
  ":root{--bg:#050a0e;--bg2:#091318;--bg3:#0d1e26;--bg4:#112530;"
  "--accent:#00e5ff;--accent2:#00ff9d;--accent3:#ff6b35;--accent4:#a855f7;"
  "--warn:#ffd60a;--danger:#ff3366;--text:#e0f4ff;--text2:#7ab3c8;--text3:#3d6978;"
  "--border:rgba(0,229,255,0.1);--glow:0 0 20px rgba(0,229,255,0.15)}"
  "*{margin:0;padding:0;box-sizing:border-box}html{scroll-behavior:smooth}"
  "body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;min-height:100vh;overflow-x:hidden}"
  ".bg-grid{position:fixed;inset:0;z-index:0;background-image:linear-gradient(rgba(0,229,255,0.025) 1px,transparent 1px),linear-gradient(90deg,rgba(0,229,255,0.025) 1px,transparent 1px);background-size:40px 40px;pointer-events:none}"
  ".bg-orb{position:fixed;border-radius:50%;filter:blur(120px);pointer-events:none;z-index:0;animation:orbF 8s ease-in-out infinite}"
  ".ob1{width:600px;height:600px;top:-200px;left:-200px;background:rgba(0,229,255,0.04)}"
  ".ob2{width:500px;height:500px;bottom:-100px;right:-100px;background:rgba(0,255,157,0.03);animation-delay:-4s}"
  "@keyframes orbF{0%,100%{transform:translate(0,0)}50%{transform:translate(30px,-30px)}}"
  "header{position:sticky;top:0;z-index:100;background:rgba(5,10,14,0.92);backdrop-filter:blur(20px);border-bottom:1px solid var(--border);padding:0 32px;height:64px;display:flex;align-items:center;justify-content:space-between}"
  ".logo{display:flex;align-items:center;gap:12px;font-size:20px;font-weight:900;color:var(--accent);letter-spacing:-0.5px}"
  ".logo-icon{width:38px;height:38px;border-radius:10px;background:linear-gradient(135deg,var(--accent),var(--accent2));display:flex;align-items:center;justify-content:center;font-size:20px;animation:lp 2s ease-in-out infinite}"
  "@keyframes lp{0%,100%{box-shadow:0 0 20px rgba(0,229,255,0.4)}50%{box-shadow:0 0 40px rgba(0,229,255,0.8)}}"
  ".hstat{display:flex;align-items:center;gap:16px}"
  ".spill{display:flex;align-items:center;gap:8px;background:var(--bg3);border:1px solid var(--border);border-radius:20px;padding:6px 14px;font-size:11px;color:var(--text2)}"
  ".sdot{width:8px;height:8px;border-radius:50%;background:var(--accent2);animation:blink 1.5s ease-in-out infinite;box-shadow:0 0 8px var(--accent2)}"
  "@keyframes blink{0%,100%{opacity:1}50%{opacity:0.3}}"
  ".tdisp{font-size:13px;color:var(--text2);font-variant-numeric:tabular-nums}"
  "main{position:relative;z-index:1;padding:24px 32px;max-width:1600px;margin:0 auto}"
  ".abanner{display:flex;align-items:center;gap:16px;border-radius:12px;padding:14px 20px;margin-bottom:24px;transition:all 0.5s}"
  ".stitle{font-size:10px;font-weight:800;letter-spacing:3px;color:var(--text3);text-transform:uppercase;margin-bottom:14px;display:flex;align-items:center;gap:10px}"
  ".stitle::after{content:'';flex:1;height:1px;background:var(--border)}"
  ".hgrid{display:grid;grid-template-columns:repeat(4,1fr);gap:16px;margin-bottom:24px}"
  ".hcard{background:rgba(9,19,24,0.9);border:1px solid var(--border);border-radius:16px;padding:24px;position:relative;overflow:hidden;cursor:pointer;transition:all 0.3s}"
  ".hcard:hover{border-color:var(--accent);transform:translateY(-2px);box-shadow:var(--glow)}"
  ".hcard.aqi-c{border-top:2px solid var(--accent3)}.hcard.co2-c{border-top:2px solid var(--accent)}"
  ".hcard.tmp-c{border-top:2px solid var(--warn)}.hcard.ctl-c{border-top:2px solid var(--accent2)}"
  ".clabel{font-size:10px;letter-spacing:2px;text-transform:uppercase;color:var(--text3);margin-bottom:8px}"
  ".cval{font-size:48px;font-weight:900;line-height:1;margin-bottom:4px}"
  ".cunit{font-size:16px;font-weight:400;color:var(--text2);margin-left:4px}"
  ".cbadge{display:inline-flex;align-items:center;padding:3px 10px;border-radius:20px;font-size:10px;font-weight:700;letter-spacing:1px;text-transform:uppercase;margin-top:8px}"
  ".bd{background:rgba(255,51,102,0.15);color:var(--danger);border:1px solid rgba(255,51,102,0.3)}"
  ".bw{background:rgba(255,214,10,0.15);color:var(--warn);border:1px solid rgba(255,214,10,0.3)}"
  ".bg{background:rgba(0,255,157,0.12);color:var(--accent2);border:1px solid rgba(0,255,157,0.25)}"
  ".bn{background:rgba(0,229,255,0.12);color:var(--accent);border:1px solid rgba(0,229,255,0.25)}"
  ".bon{background:rgba(0,255,157,0.15);color:var(--accent2);border:1px solid rgba(0,255,157,0.3)}"
  ".boff{background:rgba(61,105,120,0.3);color:var(--text3);border:1px solid var(--border)}"
  ".deco{position:absolute;right:-10px;bottom:-10px;width:100px;height:100px;border-radius:50%;opacity:0.06;pointer-events:none}"
  ".cgrid{display:grid;grid-template-columns:1fr 1fr 370px;gap:18px;margin-bottom:24px}"
  ".chcard{background:rgba(9,19,24,0.9);border:1px solid var(--border);border-radius:16px;padding:22px;overflow:hidden}"
  ".chhead{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:16px}"
  ".chtitle{font-size:14px;font-weight:700}.chsub{font-size:10px;color:var(--text3);margin-top:2px}"
  ".chcur{font-size:26px;font-weight:900;text-align:right}.chcon{position:relative;height:150px}"
  ".aipan{background:rgba(9,19,24,0.9);border:1px solid var(--border);border-radius:16px;padding:22px;display:flex;flex-direction:column;gap:12px}"
  ".aibadge{background:linear-gradient(135deg,rgba(168,85,247,0.2),rgba(0,229,255,0.1));border:1px solid rgba(168,85,247,0.3);color:var(--accent4);padding:4px 12px;border-radius:20px;font-size:10px;font-weight:700;letter-spacing:2px;text-transform:uppercase}"
  ".prow{background:var(--bg3);border:1px solid var(--border);border-radius:12px;padding:12px 14px;display:flex;justify-content:space-between;align-items:center;transition:all 0.2s}"
  ".prow:hover{border-color:rgba(168,85,247,0.3);background:var(--bg4)}"
  ".plabel{font-size:10px;color:var(--text3)}.pvals{display:flex;gap:12px;align-items:center}"
  ".pitem{text-align:center}.ptime{font-size:9px;color:var(--text3);letter-spacing:1px}"
  ".pval{font-size:13px;font-weight:800}.parr{color:var(--text3);font-size:13px}"
  ".actdisp{background:linear-gradient(135deg,rgba(0,255,157,0.06),rgba(0,229,255,0.04));border:1px solid rgba(0,255,157,0.2);border-radius:12px;padding:14px;text-align:center}"
  ".actlbl{font-size:9px;color:var(--text3);letter-spacing:2px;margin-bottom:5px}"
  ".actval{font-size:22px;font-weight:900;color:var(--accent2)}.actdesc{font-size:10px;color:var(--text2);margin-top:4px}"
  ".anomrow{display:flex;align-items:center;justify-content:space-between;background:var(--bg3);border-radius:10px;padding:12px 14px}"
  ".sgrid{display:grid;grid-template-columns:repeat(6,1fr);gap:12px;margin-bottom:24px}"
  ".scard{background:rgba(9,19,24,0.9);border:1px solid var(--border);border-radius:12px;padding:16px 14px;text-align:center;transition:all 0.3s;cursor:pointer}"
  ".scard:hover{border-color:rgba(0,229,255,0.3);box-shadow:var(--glow);transform:translateY(-2px)}"
  ".sicon{font-size:22px;margin-bottom:6px}.sname{font-size:9px;letter-spacing:2px;text-transform:uppercase;color:var(--text3);margin-bottom:5px}"
  ".sval{font-size:20px;font-weight:900;line-height:1}.sunit{font-size:10px;color:var(--text2);margin-top:2px}"
  ".sbar{height:3px;border-radius:2px;margin-top:8px;background:var(--bg3);overflow:hidden}"
  ".sbarfill{height:100%;border-radius:2px;transition:width 1s ease}"
  ".egrid{display:grid;grid-template-columns:1fr 1fr 1fr 1.6fr;gap:14px;margin-bottom:24px}"
  ".ecard{background:rgba(9,19,24,0.9);border:1px solid var(--border);border-radius:14px;padding:18px;transition:all 0.3s}"
  ".ecard:hover{border-color:rgba(255,107,53,0.3);box-shadow:0 0 20px rgba(255,107,53,0.1)}"
  ".eicon{font-size:20px;margin-bottom:8px}.elabel{font-size:9px;letter-spacing:2px;text-transform:uppercase;color:var(--text3)}"
  ".eval{font-size:24px;font-weight:900;margin:5px 0}.eunit{font-size:11px;color:var(--text2)}"
  ".cbncard{background:linear-gradient(135deg,rgba(0,255,157,0.06),rgba(9,19,24,0.9));border:1px solid rgba(0,255,157,0.15);border-radius:14px;padding:18px}"
  ".cbntitle{font-size:10px;color:var(--accent2);letter-spacing:2px;text-transform:uppercase;margin-bottom:10px}"
  ".trees{display:flex;gap:3px;flex-wrap:wrap;min-height:26px}"
  ".ti{font-size:17px;transition:all 0.3s;display:inline-block}.ti:hover{transform:scale(1.3)}"
  ".osec{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-bottom:24px}"
  ".ocard{background:rgba(9,19,24,0.9);border:1px solid var(--border);border-radius:14px;padding:18px}"
  ".ovis{display:flex;gap:12px;align-items:center;margin-top:10px}.oprs{font-size:38px;transition:all 0.5s}"
  ".mtogg{display:flex;gap:8px;margin-top:10px}"
  ".mbtn{padding:8px 20px;border-radius:8px;font-size:12px;font-weight:700;cursor:pointer;border:1px solid var(--border);letter-spacing:1px;transition:all 0.2s;background:var(--bg3);color:var(--text3)}"
  ".mauto{background:rgba(0,229,255,0.1);border-color:var(--accent);color:var(--accent)}"
  ".mman{background:rgba(255,107,53,0.1);border-color:var(--accent3);color:var(--accent3)}"
  ".accard{background:rgba(9,19,24,0.9);border:1px solid var(--border);border-radius:14px;padding:18px;display:flex;flex-direction:column}"
  ".acvis{flex:1;display:flex;align-items:center;justify-content:center;margin:14px 0}"
  ".fancon{position:relative;width:100px;height:100px;display:flex;align-items:center;justify-content:center}"
  ".fring{position:absolute;inset:0;border-radius:50%;border:2px solid rgba(0,255,157,0.2)}"
  ".ficon{font-size:60px;transition:all 0.5s}"
  ".fspin{animation:fsp 0.8s linear infinite}"
  "@keyframes fsp{from{transform:rotate(0)}to{transform:rotate(360deg)}}"
  ".fglow{position:absolute;inset:-10px;border-radius:50%;border:1px solid rgba(0,255,157,0.15);animation:rp 2s ease-in-out infinite;display:none}"
  "@keyframes rp{0%,100%{opacity:0.5;transform:scale(1)}50%{opacity:1;transform:scale(1.05)}}"
  ".acinfo{text-align:center}.acstate{font-size:20px;font-weight:900}.acreason{font-size:10px;color:var(--text3);margin-top:4px}"
  ".histcard{background:rgba(9,19,24,0.9);border:1px solid var(--border);border-radius:16px;padding:22px;margin-bottom:24px}"
  ".histhead{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px}"
  ".htabs{display:flex;gap:6px}"
  ".htab{padding:5px 14px;border-radius:8px;font-size:11px;font-weight:600;background:var(--bg3);border:1px solid var(--border);color:var(--text2);cursor:pointer;transition:all 0.2s}"
  ".htab.act{background:rgba(0,229,255,0.1);border-color:var(--accent);color:var(--accent)}"
  ".histcon{position:relative;height:200px}"
  "footer{border-top:1px solid var(--border);padding:18px 32px;display:flex;justify-content:space-between;align-items:center;position:relative;z-index:1}"
  ".ftxt{font-size:10px;color:var(--text3)}.fdots{display:flex;gap:6px}"
  ".fd{width:6px;height:6px;border-radius:50%;background:var(--bg3)}.fd.act{background:var(--accent);box-shadow:0 0 8px var(--accent)}"
  ".lovl{position:fixed;inset:0;background:rgba(5,10,14,0.97);z-index:999;display:flex;align-items:center;justify-content:center;flex-direction:column;gap:24px;transition:opacity 0.5s}"
  ".llog{font-size:52px;animation:lp 1s ease-in-out infinite}"
  ".ltxt{font-size:13px;color:var(--accent);letter-spacing:4px;font-weight:600}"
  ".lbar{width:220px;height:2px;background:var(--bg3);border-radius:1px;overflow:hidden}"
  ".lfill{height:100%;background:linear-gradient(90deg,var(--accent),var(--accent2));border-radius:1px;animation:lf 1.8s ease forwards}"
  "@keyframes lf{from{width:0}to{width:100%}}"
  "::-webkit-scrollbar{width:6px}::-webkit-scrollbar-track{background:var(--bg)}::-webkit-scrollbar-thumb{background:var(--bg4);border-radius:3px}"
  "@media(max-width:1200px){.hgrid{grid-template-columns:repeat(2,1fr)}.cgrid{grid-template-columns:1fr 1fr}.sgrid{grid-template-columns:repeat(3,1fr)}.egrid{grid-template-columns:1fr 1fr}}"
  "@media(max-width:768px){main{padding:14px 16px}.hgrid{grid-template-columns:1fr 1fr}.cgrid{grid-template-columns:1fr}.sgrid{grid-template-columns:repeat(2,1fr)}.egrid{grid-template-columns:1fr 1fr}.osec{grid-template-columns:1fr}}"
  "</style></head><body>"));

  /* ── BODY HTML ── */
  server.sendContent(F(
  "<div class='lovl' id='lo'><div class='llog'>&#127758;</div><div class='ltxt'>AIRMIND INITIALIZING</div><div class='lbar'><div class='lfill'></div></div></div>"
  "<div class='bg-grid'></div><div class='bg-orb ob1'></div><div class='bg-orb ob2'></div>"
  "<header>"
  "<div class='logo'><div class='logo-icon'>&#127758;</div>AirMind</div>"
  "<div class='hstat'>"
  "<div class='spill'><div class='sdot'></div>LIVE &middot; ESP32</div>"
  "<div class='spill'><div class='sdot' style='background:var(--accent4)'></div>AI ACTIVE</div>"
  "<div class='tdisp' id='clk'>--:--:--</div>"
  "</div></header>"
  "<main>"

  /* ALERT BANNER */
  "<div class='abanner' id='ab' style='background:linear-gradient(90deg,rgba(255,214,10,0.08),rgba(255,214,10,0.03));border:1px solid rgba(255,214,10,0.25)'>"
  "<span style='font-size:20px' id='abIcon'>&#9888;&#65039;</span>"
  "<div style='font-size:13px;color:var(--warn)'>AQI <span id='abAqi' style='font-weight:700'>&mdash;</span> &mdash; <span id='abMsg'>Initializing...</span></div>"
  "<span class='cbadge bw' id='abBadge' style='margin-left:auto'>CHECKING</span>"
  "</div>"

  /* HERO CARDS */
  "<div class='stitle'>Live Readings</div>"
  "<div class='hgrid'>"
  "<div class='hcard aqi-c'><div class='clabel'>Air Quality Index</div><div class='cval' id='aqiV' style='color:var(--accent3)'>&mdash;</div><span class='cbadge bn' id='aqiB'>&mdash;</span><div style='font-size:11px;color:var(--text3);margin-top:8px' id='aqiL'>&mdash;</div><svg class='deco' viewBox='0 0 100 100'><circle cx='50' cy='50' r='40' fill='var(--accent3)'/></svg></div>"
  "<div class='hcard co2-c'><div class='clabel'>CO&#8322; Level</div><div class='cval' id='co2V' style='color:var(--accent)'>&mdash;<span class='cunit'>ppm</span></div><span class='cbadge bn' id='co2B'>&mdash;</span><div style='font-size:11px;color:var(--text3);margin-top:8px' id='co2L'>&mdash;</div><svg class='deco' viewBox='0 0 100 100'><circle cx='50' cy='50' r='40' fill='var(--accent)'/></svg></div>"
  "<div class='hcard tmp-c'><div class='clabel'>Temperature</div><div class='cval' id='tmpV' style='color:var(--warn)'>&mdash;<span class='cunit'>&#176;C</span></div><div style='margin-top:8px'><div style='font-size:10px;color:var(--text3);letter-spacing:2px'>HUMIDITY</div><div style='font-size:24px;font-weight:900;color:var(--accent)'><span id='humV'>&mdash;</span><span style='font-size:14px;color:var(--text2)'>%</span></div></div><svg class='deco' viewBox='0 0 100 100'><circle cx='50' cy='50' r='40' fill='var(--warn)'/></svg></div>"
  "<div class='hcard ctl-c'><div class='clabel'>AirCtrl Status</div><div class='cval' id='acV' style='color:var(--text3)'>&mdash;</div><span class='cbadge boff' id='acB'>&mdash;</span><div style='font-size:11px;color:var(--text3);margin-top:8px'>Mode: <span id='modeL' style='color:var(--accent)'>&mdash;</span> &middot; Occ: <span id='occL' style='color:var(--accent2)'>&mdash;</span></div><svg class='deco' viewBox='0 0 100 100'><circle cx='50' cy='50' r='40' fill='var(--accent2)'/></svg></div>"
  "</div>"

  /* CHARTS + AI */
  "<div class='stitle'>Trend Analysis &amp; AI Predictions</div>"
  "<div class='cgrid'>"
  "<div class='chcard'><div class='chhead'><div><div class='chtitle'>AQI Trend</div><div class='chsub'>LAST 30 READINGS &middot; 2s INTERVAL</div></div><div class='chcur' style='color:var(--accent3)' id='aqiCC'>&mdash;</div></div><div class='chcon'><canvas id='aqiCh'></canvas></div></div>"
  "<div class='chcard'><div class='chhead'><div><div class='chtitle'>CO&#8322; Trend</div><div class='chsub'>LAST 30 READINGS &middot; 2s INTERVAL</div></div><div class='chcur' style='color:var(--accent)' id='co2CC'>&mdash;</div></div><div class='chcon'><canvas id='co2Ch'></canvas></div></div>"
  "<div class='aipan'>"
  "<div><span class='aibadge'>&#10022; AI Engine</span></div>"
  "<div class='prow'><div class='plabel'>AQI FORECAST</div><div class='pvals'><div class='pitem'><div class='ptime'>NOW</div><div class='pval' id='pAN' style='color:var(--accent3)'>&mdash;</div></div><div class='parr'>&#8594;</div><div class='pitem'><div class='ptime'>+10s</div><div class='pval' id='pA10' style='color:var(--accent3)'>&mdash;</div></div><div class='pitem'><div class='ptime'>+30s</div><div class='pval' id='pA30' style='color:var(--accent3)'>&mdash;</div></div><div class='pitem'><div class='ptime'>+60s</div><div class='pval' id='pA60' style='color:var(--accent3)'>&mdash;</div></div></div></div>"
  "<div class='prow'><div class='plabel'>CO&#8322; FORECAST</div><div class='pvals'><div class='pitem'><div class='ptime'>NOW</div><div class='pval' id='pCN' style='color:var(--accent)'>&mdash;</div></div><div class='parr'>&#8594;</div><div class='pitem'><div class='ptime'>+10s</div><div class='pval' id='pC10' style='color:var(--accent)'>&mdash;</div></div><div class='pitem'><div class='ptime'>+30s</div><div class='pval' id='pC30' style='color:var(--accent)'>&mdash;</div></div><div class='pitem'><div class='ptime'>+60s</div><div class='pval' id='pC60' style='color:var(--accent)'>&mdash;</div></div></div></div>"
  "<div class='actdisp'><div class='actlbl'>AI RECOMMENDATION</div><div class='actval' id='aiAct'>&mdash;</div><div class='actdesc' id='aiActD'>Generating prediction...</div></div>"
  "<div class='anomrow'><div><div style='font-size:10px;letter-spacing:2px;color:var(--text3);margin-bottom:4px'>ANOMALY DETECTION</div><div style='font-size:18px;font-weight:900' id='anomSt'>&mdash;</div></div><div style='font-size:26px' id='anomIc'>&#128269;</div></div>"
  "</div></div>"

  /* SENSOR ARRAY */
  "<div class='stitle'>Sensor Array</div>"
  "<div class='sgrid'>"
  "<div class='scard'><div class='sicon'>&#129514;</div><div class='sname'>NH&#8323;</div><div class='sval' id='nh3V' style='color:var(--accent4)'>&mdash;</div><div class='sunit'>ppm</div><div class='sbar'><div class='sbarfill' id='nh3B' style='background:var(--accent4)'></div></div></div>"
  "<div class='scard'><div class='sicon'>&#9879;</div><div class='sname'>Benzene</div><div class='sval' id='bzV' style='color:var(--danger)'>&mdash;</div><div class='sunit'>ppm</div><div class='sbar'><div class='sbarfill' id='bzB' style='background:var(--danger)'></div></div></div>"
  "<div class='scard'><div class='sicon'>&#128168;</div><div class='sname'>CO</div><div class='sval' id='coV' style='color:var(--warn)'>&mdash;</div><div class='sunit'>ppm</div><div class='sbar'><div class='sbarfill' id='coB' style='background:var(--warn)'></div></div></div>"
  "<div class='scard'><div class='sicon'>&#9889;</div><div class='sname'>Voltage</div><div class='sval' id='vltV' style='color:var(--accent)'>&mdash;</div><div class='sunit'>V</div><div class='sbar'><div class='sbarfill' id='vltB' style='background:var(--accent);width:70%'></div></div></div>"
  "<div class='scard'><div class='sicon'>&#128268;</div><div class='sname'>Current</div><div class='sval' id='curV' style='color:var(--accent3)'>&mdash;</div><div class='sunit'>mA</div><div class='sbar'><div class='sbarfill' id='curB' style='background:var(--accent3)'></div></div></div>"
  "<div class='scard'><div class='sicon'>&#128161;</div><div class='sname'>Power</div><div class='sval' id='pwrV' style='color:var(--accent2)'>&mdash;</div><div class='sunit'>W</div><div class='sbar'><div class='sbarfill' id='pwrB' style='background:var(--accent2)'></div></div></div>"
  "</div>"

  /* ENERGY */
  "<div class='stitle'>Energy &amp; Carbon Footprint</div>"
  "<div class='egrid'>"
  "<div class='ecard'><div class='eicon'>&#9889;</div><div class='elabel'>Energy Used</div><div class='eval' id='engV' style='color:var(--accent)'>&mdash;</div><div class='eunit'>kWh</div></div>"
  "<div class='ecard'><div class='eicon'>&#128176;</div><div class='elabel'>Cost Estimate</div><div class='eval' id='cstV' style='color:var(--accent2)'>&mdash;</div><div class='eunit'>INR (approx)</div></div>"
  "<div class='ecard'><div class='eicon'>&#127758;</div><div class='elabel'>CO&#8322; Emitted</div><div class='eval' id='cemV' style='color:var(--accent3)'>&mdash;</div><div class='eunit'>g CO&#8322;</div></div>"
  "<div class='cbncard'><div class='cbntitle'>&#127795; Tree Equivalent</div><div style='font-size:11px;color:var(--text2);margin-bottom:8px'>Minutes of CO&#8322; absorption needed:</div><div class='trees' id='treeCon'><span class='ti'>&#127807;</span></div><div style='margin-top:8px;font-size:10px;color:var(--text3)' id='treeTxt'>&mdash;</div></div>"
  "</div>"

  /* OCCUPANCY + AIRCTRL */
  "<div class='stitle'>Occupancy &amp; Control</div>"
  "<div class='osec'>"
  "<div class='ocard'><div style='font-size:13px;font-weight:700'>Occupancy Monitor</div><div style='font-size:11px;color:var(--text3);margin-bottom:4px'>PIR &middot; GPIO 27</div><div class='ovis'><div class='oprs' id='occPrs'>&#128694;</div><div><div style='font-size:26px;font-weight:900' id='occSt'>&mdash;</div><div style='font-size:12px;color:var(--text2)' id='occDs'>&mdash;</div></div></div><div style='font-size:10px;color:var(--text3);margin-bottom:8px;letter-spacing:2px'>CONTROL MODE</div><div class='mtogg'><div class='mbtn' id='mAuto' onclick='setMode(\"AUTO\")'>AUTO</div><div class='mbtn' id='mMan' onclick='setMode(\"MANUAL\")'>MANUAL</div></div></div>"
  "<div class='accard'><div style='font-size:13px;font-weight:700'>AirCtrl Output</div><div style='font-size:11px;color:var(--text3)'>Relay &middot; GPIO 12</div><div class='acvis'><div class='fancon'><div class='fring'></div><div class='fglow' id='fGlow'></div><div class='ficon' id='fIco'>&#127744;</div></div></div><div class='acinfo'><div class='acstate' id='acSt' style='color:var(--text3)'>&mdash;</div><div class='acreason' id='acRs'>&mdash;</div></div></div>"
  "</div>"

  /* HISTORY */
  "<div class='stitle'>History</div>"
  "<div class='histcard'><div class='histhead'><div style='font-size:14px;font-weight:700'>Environmental Timeline</div><div class='htabs'><div class='htab act' onclick='switchTab(\"aqi\",this)'>AQI</div><div class='htab' onclick='switchTab(\"co2\",this)'>CO&#8322;</div><div class='htab' onclick='switchTab(\"temp\",this)'>Temp</div><div class='htab' onclick='switchTab(\"humidity\",this)'>Humidity</div></div></div><div class='histcon'><canvas id='histCh'></canvas></div></div>"
  "</main>"
  "<footer><div class='ftxt'>AIRMIND v2.0 &middot; ESP32 Web Server &middot; <span id='fupt'>Initializing...</span></div><div class='fdots'><div class='fd act' id='d1'></div><div class='fd' id='d2'></div><div class='fd' id='d3'></div></div></footer>"
  ));

  /* ── JAVASCRIPT ── */
  server.sendContent(F("<script>"
  "Chart.defaults.color='#3d6978';Chart.defaults.font.size=10;"
  "const S={hist:{aqi:[],co2:[],temp:[],humidity:[],labels:[]},n:0};"
  "const ADESC={'KEEP_OFF':'Air quality OK - system standby','TURN_ON':'Immediate activation recommended','RUN_2MIN':'Run ventilation for 2 minutes','RUN_5MIN':'Extended ventilation needed'};"
  "function gLabel(v){if(v<=50)return'Good';if(v<=100)return'Moderate';if(v<=150)return'Unhealthy Sensitive';if(v<=200)return'Unhealthy';if(v<=300)return'Very Unhealthy';return'Hazardous'}"
  "function co2L(v){if(v<800)return'Normal CO2';if(v<1200)return'Elevated CO2';if(v<1500)return'High CO2 Level';return'Very High CO2'}"
  "function gColor(v){if(v<=50)return'#00ff9d';if(v<=100)return'#ffd60a';if(v<=150)return'#ff8c00';if(v<=200)return'#ff6b35';return'#ff3366'}"
  "function gBadge(v){if(v<=50)return'bg';if(v<=100)return'bn';if(v<=150)return'bw';return'bd'}"
  "function aiCalc(d){const dr=(Math.random()-0.45)*4;return{aqi_now:d.aqi,aqi_10sec:(+d.aqi+dr).toFixed(0),aqi_30sec:(+d.aqi+dr*2).toFixed(0),aqi_60sec:(+d.aqi+dr*3.5).toFixed(0),co2_now:d.co2,co2_10sec:(+d.co2+dr*8).toFixed(0),co2_30sec:(+d.co2+dr*16).toFixed(0),co2_60sec:(+d.co2+dr*25).toFixed(0),action:d.airCtrl?'RUN_2MIN':'KEEP_OFF',anomaly:+d.aqi>300?'ANOMALY':'NORMAL'};}"
  "function mkSp(id,col){const ctx=document.getElementById(id).getContext('2d');const g=ctx.createLinearGradient(0,0,0,150);g.addColorStop(0,col+'33');g.addColorStop(1,col+'00');return new Chart(ctx,{type:'line',data:{labels:Array(30).fill(''),datasets:[{data:Array(30).fill(null),borderColor:col,borderWidth:2,backgroundColor:g,fill:true,tension:0.4,pointRadius:0}]},options:{responsive:true,maintainAspectRatio:false,animation:{duration:300},plugins:{legend:{display:false}},scales:{x:{display:false},y:{grid:{color:'rgba(255,255,255,0.04)'},border:{display:false},ticks:{color:'#3d6978',maxTicksLimit:4}}}}});}"
  "function mkHist(){const ctx=document.getElementById('histCh').getContext('2d');const c='#00e5ff';const g=ctx.createLinearGradient(0,0,0,200);g.addColorStop(0,c+'44');g.addColorStop(1,c+'00');return new Chart(ctx,{type:'line',data:{labels:[],datasets:[{label:'AQI',data:[],borderColor:c,borderWidth:2,backgroundColor:g,fill:true,tension:0.4,pointRadius:0}]},options:{responsive:true,maintainAspectRatio:false,animation:{duration:500},plugins:{legend:{display:false}},scales:{x:{grid:{color:'rgba(255,255,255,0.03)'},ticks:{color:'#3d6978',maxTicksLimit:8},border:{display:false}},y:{grid:{color:'rgba(255,255,255,0.04)'},border:{display:false},ticks:{color:'#3d6978'}}}}});}"
  "let aqiSp,co2Sp,histCh,curTab='aqi';"
  "function initCharts(){aqiSp=mkSp('aqiCh','#ff6b35');co2Sp=mkSp('co2Ch','#00e5ff');histCh=mkHist();}"
  "function switchTab(t,el){curTab=t;document.querySelectorAll('.htab').forEach(x=>x.classList.remove('act'));el.classList.add('act');updHist();}"
  "function updHist(){const cols={aqi:'#ff6b35',co2:'#00e5ff',temp:'#ffd60a',humidity:'#a855f7'};const labs={aqi:'AQI',co2:'CO2(ppm)',temp:'Temp(C)',humidity:'Humidity(%)'};const c=cols[curTab];const ctx=document.getElementById('histCh').getContext('2d');const g=ctx.createLinearGradient(0,0,0,200);g.addColorStop(0,c+'44');g.addColorStop(1,c+'00');histCh.data.labels=[...S.hist.labels];histCh.data.datasets[0].data=[...S.hist[curTab]];histCh.data.datasets[0].borderColor=c;histCh.data.datasets[0].backgroundColor=g;histCh.data.datasets[0].label=labs[curTab];histCh.update('none');}"
  "const $=id=>document.getElementById(id);"
  "function updUI(d,ai){"
  "const c=gColor(+d.aqi);"
  "$('aqiV').textContent=d.aqi;$('aqiV').style.color=c;"
  "$('aqiB').textContent=gLabel(+d.aqi);$('aqiB').className='cbadge '+gBadge(+d.aqi);"
  "$('aqiL').textContent=d.airQuality;$('aqiCC').textContent=d.aqi;"
  "$('co2V').innerHTML=d.co2+'<span class=\"cunit\">ppm</span>';"
  "$('co2B').textContent=co2L(+d.co2);$('co2B').className='cbadge '+(+d.co2>1500?'bd':+d.co2>1000?'bw':'bg');"
  "$('co2L').textContent=co2L(+d.co2);$('co2CC').textContent=d.co2;"
  "$('tmpV').innerHTML=d.temp+'<span class=\"cunit\">&#176;C</span>';$('humV').textContent=d.humidity;"
  "const on=d.airCtrl==1;"
  "$('acV').textContent=on?'ON':'OFF';$('acV').style.color=on?'var(--accent2)':'var(--text3)';"
  "$('acB').textContent=on?'ACTIVE':'STANDBY';$('acB').className='cbadge '+(on?'bon':'boff');"
  "$('modeL').textContent=d.mode;$('occL').textContent=d.occupancy?'PRESENT':'ABSENT';"
  "$('nh3V').textContent=d.nh3;$('nh3B').style.width=Math.min(100,+d.nh3/2)+'%';"
  "$('bzV').textContent=d.benzene;$('bzB').style.width=Math.min(100,+d.benzene*5)+'%';"
  "$('coV').textContent=d.co;$('coB').style.width=Math.min(100,+d.co*10)+'%';"
  "$('vltV').textContent=d.voltage;$('curV').textContent=d.current;$('curB').style.width=Math.min(100,+d.current/5)+'%';"
  "$('pwrV').textContent=d.power;$('pwrB').style.width=Math.min(100,+d.power*10)+'%';"
  "$('engV').textContent=parseFloat(d.energy).toFixed(4);$('cstV').textContent='Rs.'+parseFloat(d.cost).toFixed(4);"
  "const co2g=(parseFloat(d.energy)*820).toFixed(1);$('cemV').textContent=co2g;updTrees(+co2g);"
  "const occ=d.occupancy==1;"
  "$('occPrs').style.opacity=occ?'1':'0.3';$('occSt').textContent=occ?'PRESENT':'ABSENT';"
  "$('occSt').style.color=occ?'var(--accent2)':'var(--text3)';"
  "$('occDs').textContent=occ?'Motion detected - room occupied':'No motion - room empty';"
  "$('mAuto').className='mbtn'+(d.mode==='AUTO'?' mauto':'');"
  "$('mMan').className='mbtn'+(d.mode==='MANUAL'?' mman':'');"
  "$('fIco').className='ficon'+(on?' fspin':'');"
  "$('acSt').textContent=on?'RUNNING':'STANDBY';$('acSt').style.color=on?'var(--accent2)':'var(--text3)';"
  "$('fGlow').style.display=on?'block':'none';"
  "$('acRs').textContent=on?'AQI '+d.aqi+' > threshold':'Awaiting activation conditions';"
  /* ALERT */
  "$('abAqi').textContent=d.aqi;"
  "if(+d.aqi>200){$('ab').style.cssText='display:flex;align-items:center;gap:16px;background:linear-gradient(90deg,rgba(255,51,102,0.1),rgba(255,51,102,0.03));border:1px solid rgba(255,51,102,0.3);border-radius:12px;padding:14px 20px;margin-bottom:24px';$('abMsg').textContent='Very Unhealthy - AirCtrl activated';$('abBadge').textContent='ALERT';$('abBadge').className='cbadge bd';$('abIcon').innerHTML='&#128680;';}"
  "else if(+d.aqi>100){$('ab').style.cssText='display:flex;align-items:center;gap:16px;background:linear-gradient(90deg,rgba(255,214,10,0.08),rgba(255,214,10,0.03));border:1px solid rgba(255,214,10,0.25);border-radius:12px;padding:14px 20px;margin-bottom:24px';$('abMsg').textContent='Moderate - monitoring closely';$('abBadge').textContent='CAUTION';$('abBadge').className='cbadge bw';$('abIcon').innerHTML='&#9888;&#65039;';}"
  "else{$('ab').style.cssText='display:flex;align-items:center;gap:16px;background:linear-gradient(90deg,rgba(0,255,157,0.06),rgba(0,255,157,0.02));border:1px solid rgba(0,255,157,0.2);border-radius:12px;padding:14px 20px;margin-bottom:24px';$('abMsg').textContent='Air quality within acceptable range';$('abBadge').textContent='NORMAL';$('abBadge').className='cbadge bg';$('abIcon').innerHTML='&#9989;';}"
  /* SPARKLINES */
  "const sp=aqiSp.data.datasets[0].data;sp.push(+d.aqi);if(sp.length>30)sp.shift();aqiSp.update('none');"
  "const sp2=co2Sp.data.datasets[0].data;sp2.push(+d.co2);if(sp2.length>30)sp2.shift();co2Sp.update('none');"
  /* AI PANEL */
  "$('pAN').textContent=ai.aqi_now;$('pA10').textContent=ai.aqi_10sec;$('pA30').textContent=ai.aqi_30sec;$('pA60').textContent=ai.aqi_60sec;"
  "$('pCN').textContent=ai.co2_now;$('pC10').textContent=ai.co2_10sec;$('pC30').textContent=ai.co2_30sec;$('pC60').textContent=ai.co2_60sec;"
  "$('aiAct').textContent=ai.action;$('aiAct').style.color=ai.action==='KEEP_OFF'?'var(--text2)':'var(--accent2)';"
  "$('aiActD').textContent=ADESC[ai.action]||'';"
  "const isA=ai.anomaly==='ANOMALY';$('anomSt').textContent=ai.anomaly;$('anomSt').style.color=isA?'var(--danger)':'var(--accent2)';$('anomIc').innerHTML=isA?'&#128680;':'&#9989;';"
  "}"
  "function updTrees(g){const n=Math.max(1,Math.round(g/0.5));const con=$('treeCon');con.innerHTML='';const em=['&#127795;','&#127794;','&#127796;','&#127797;'];const d=Math.min(n,20);for(let i=0;i<d;i++){const s=document.createElement('span');s.className='ti';s.innerHTML=em[i%4];con.appendChild(s)}$('treeTxt').textContent='~'+n+' tree-min to offset '+g+'g CO2';}"
  "function pushHist(d){const now=new Date();const lbl=now.getHours().toString().padStart(2,'0')+':'+now.getMinutes().toString().padStart(2,'0')+':'+now.getSeconds().toString().padStart(2,'0');['aqi','co2','temp','humidity'].forEach(k=>{S.hist[k].push(+d[k]);if(S.hist[k].length>60)S.hist[k].shift()});S.hist.labels.push(lbl);if(S.hist.labels.length>60)S.hist.labels.shift();updHist();}"
  "async function update(){S.n++;try{const r=await fetch('/api/data',{signal:AbortSignal.timeout(2000)});const d=await r.json();updUI(d,aiCalc(d));pushHist(d);}catch(e){console.log(e);}const di=S.n%3;['d1','d2','d3'].forEach((id,i)=>{$(id).className='fd'+(i===di?' act':'')});$('fupt').textContent='Updated '+new Date().toLocaleTimeString();}"
  "async function setMode(m){try{await fetch('/api/mode?set='+m);}catch(e){}}"
  "function updClk(){$('clk').textContent=new Date().toLocaleTimeString('en-US',{hour12:false})}"
  "window.addEventListener('load',()=>{initCharts();updClk();setInterval(updClk,1000);"
  "setTimeout(()=>{const o=$('lo');o.style.opacity='0';setTimeout(()=>o.style.display='none',500)},1800);"
  "setTimeout(()=>{update();setInterval(update,2000)},2000);});"
  "</script></body></html>"));
}

/* ══════════════════════════════
   SETUP
   ══════════════════════════════ */
void setup(){
  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(PIR_PIN,INPUT);
  pinMode(FAN_PIN,OUTPUT);    digitalWrite(FAN_PIN,LOW);
  pinMode(BUZZER_PIN,OUTPUT); digitalWrite(BUZZER_PIN,LOW);
  pinMode(RED_LED,OUTPUT);    digitalWrite(RED_LED,LOW);
  pinMode(GREEN_LED,OUTPUT);  digitalWrite(GREEN_LED,LOW);
  pinMode(BLUE_LED,OUTPUT);   digitalWrite(BLUE_LED,LOW);
  Wire.begin(21,22);
  dht.begin();
  lcd.init(); lcd.backlight(); lcd.clear();
  if(!ina219.begin()){Serial.println("INA219 not found");inaErr=true;}
  loadR0();
  startMs=millis(); prevEnergyMs=millis();

  WiFi.mode(WIFI_STA); WiFi.begin(ssid,password);
  lcd.setCursor(0,0); lcd.print("Connecting WiFi");
  Serial.print("Connecting");
  while(WiFi.status()!=WL_CONNECTED){delay(500);Serial.print(".");}
  Serial.println();

  String ip=WiFi.localIP().toString();
  Serial.println("====================================");
  Serial.println("  AirMind ESP32 Cyberpunk Dashboard");
  Serial.println("====================================");
  Serial.print("  Open browser: http://"); Serial.println(ip);
  Serial.print("  Live JSON:    http://"); Serial.println(ip+"/api/data");
  Serial.println("====================================");
  lcd.clear(); lcd.setCursor(0,0); lcd.print("AirMind Online!"); lcd.setCursor(0,1); lcd.print(ip);
  delay(3000);

  server.on("/",         handleRoot);
  server.on("/api/data", handleData);
  server.on("/api/mode", handleMode);
  server.begin();
  Serial.println("Web server running!");
}

/* ══════════════════════════════
   LOOP
   ══════════════════════════════ */
void loop(){
  server.handleClient();
  unsigned long now=millis();
  if(now-lastSensorMs>=2000){lastSensorMs=now;updateOcc();readSensors();updateFan();updateLEDs();handleBuzz();
    Serial.print("[AirMind] AQI=");Serial.print(AQI);Serial.print(" CO2=");Serial.print((int)co2ppm);Serial.print(" T=");Serial.print(temp,1);Serial.print(" Fan=");Serial.println(fanState?"ON":"OFF");}
  if(now-lastLCDMs>=2000){lastLCDMs=now;updateLCD();}
}
