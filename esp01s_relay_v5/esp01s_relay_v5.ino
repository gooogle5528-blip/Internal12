

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <time.h>
#include <sys/time.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>  

ADC_MODE(ADC_VCC);                       

// ======== CAU HINH ========
const char* WIFI_SSID = "--------";
const char* WIFI_PASS = "--------";
const char* HOSTNAME  = "esp-relay";
const char* OTA_PASS  = "12345678";      

const uint8_t RELAY_PIN = 0;            
const bool COIL_ACTIVE_LOW = false;      

const int  SENSE_PIN = -1;               
const bool SENSE_ACTIVE_LOW = true;      

const int  BTN_PIN = -1;                
const long TZ_OFFSET = 7 * 3600;        
const uint32_t RAM_TOTAL = 81920;       

// --- Dieu khien tu xa (de MQTT_HOST = "" de tat) ---
const char*    MQTT_HOST = "";           
const uint16_t MQTT_PORT = 8883;         
const char*    MQTT_USER = "";
const char*    MQTT_PASS = "";
const char*    MQTT_BASE = "den/nha-doi-chuoi-nay";   
// ==========================

ESP8266WebServer server(80);
BearSSL::WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);
String tCmd, tState, tOnline, tInfo;

struct Cfg {
  uint8_t  magic;
  uint8_t  coil;
  uint8_t  lampEst;      
  uint8_t  schedEn;
  uint32_t onSec;        
  uint32_t offSec;      
  uint8_t  idleMin;     
} cfg;

bool coilOn = false;
bool sleeping = false;
bool apFallback = false;
unsigned long timerAt = 0;              
uint8_t timerSet = 0;                   
unsigned long lastAct = 0;


const uint8_t HN = 90;
int8_t   hR[HN];                         

uint16_t hV[HN];                     
uint8_t  hHead = 0, hN = 0;

void histPush() {
  hR[hHead] = (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : 0;
  hV[hHead] = (uint16_t)ESP.getVcc();
  hHead = (hHead + 1) % HN;
  if (hN < HN) hN++;
}

// ---------- luu cai dat ----------
void saveCfg() { EEPROM.put(0, cfg); EEPROM.commit(); }
void loadCfg() {
  EEPROM.get(0, cfg);
  if (cfg.magic != 0xA7) {
    cfg = {0xA7, 0, 1, 0, 18UL * 3600, 22UL * 3600, 5};
    saveCfg();
  }
}

// ---------- relay / den ----------
void driveCoil(bool on) {
  coilOn = on;
  digitalWrite(RELAY_PIN, (on != COIL_ACTIVE_LOW) ? HIGH : LOW);
}

void toggleLamp() {
  driveCoil(!coilOn);
  cfg.coil = coilOn ? 1 : 0;
  cfg.lampEst = cfg.coil;             
  saveCfg();
}

int sensed() {                          
  if (SENSE_PIN < 0) return -1;
  bool low = (digitalRead(SENSE_PIN) == LOW);
  return (low == SENSE_ACTIVE_LOW) ? 1 : 0;
}

bool lampIsOn() {
  int s = sensed();
  return (s >= 0) ? (s == 1) : (cfg.lampEst == 1);
}

void setLamp(bool want) {
  if (lampIsOn() != want) { toggleLamp(); delay(120); }
}

void startAP() {
  apFallback = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP-Relay", "12345678");
}

// ---------- che do ngu ----------
void setAwake() {
  sleeping = false;
  if (WiFi.getMode() == WIFI_STA) WiFi.setSleepMode(WIFI_NONE_SLEEP);
}
void setSleepy() {
  sleeping = true;
  if (WiFi.getMode() == WIFI_STA) WiFi.setSleepMode(WIFI_LIGHT_SLEEP, 3);
}
void touch() {
  lastAct = millis();
  if (sleeping) setAwake();
}

// ---------- dieu khien tu xa (MQTT) ----------
bool mqttEnabled() { return MQTT_HOST[0] != 0; }

void mqttPublishState() {
  if (!mqtt.connected()) return;
  mqtt.publish(tState.c_str(), lampIsOn() ? "on" : "off", true);
}

void mqttPublishInfo() {
  if (!mqtt.connected()) return;
  String j = "{\"sensed\":";
  j += (sensed() >= 0) ? "true" : "false";
  j += ",\"heap\":";  j += ESP.getFreeHeap();
  j += ",\"vcc\":";   j += ESP.getVcc();
  j += ",\"rssi\":";  j += WiFi.RSSI();
  j += ",\"up\":";    j += (unsigned long)(millis() / 1000);
  j += "}";
  mqtt.publish(tInfo.c_str(), j.c_str(), true);
}

void onMqttMsg(char* topic, byte* payload, unsigned int len) {
  String m;
  for (unsigned int i = 0; i < len && i < 16; i++) m += (char)payload[i];
  m.trim();
  m.toLowerCase();
  touch();                              
  if (m == "on" || m == "1" || m == "true")        setLamp(true);
  else if (m == "off" || m == "0" || m == "false") setLamp(false);
  else if (m == "toggle")                          toggleLamp();
}

void mqttSetup() {
  if (!mqttEnabled()) return;
  tCmd    = String(MQTT_BASE) + "/cmd";
  tState  = String(MQTT_BASE) + "/state";
  tOnline = String(MQTT_BASE) + "/online";
  tInfo   = String(MQTT_BASE) + "/info";
  tlsClient.setInsecure();              
  tlsClient.setBufferSizes(512, 512);   
  tlsClient.setTimeout(4000);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMsg);
  mqtt.setBufferSize(384);
  mqtt.setKeepAlive(30);
}

void mqttLoop() {
  if (!mqttEnabled() || apFallback || WiFi.status() != WL_CONNECTED) return;
  static unsigned long lastTry = 0, lastPub = 0;
  static int lastLamp = -1;
  unsigned long now = millis();

  if (mqtt.connected()) {
    mqtt.loop();
    int cur = lampIsOn() ? 1 : 0;
    if (cur != lastLamp || now - lastPub > 60000UL) {  
      lastLamp = cur;
      lastPub = now;
      mqttPublishState();
      mqttPublishInfo();
    }
    return;
  }
  if (lastTry != 0 && now - lastTry < 15000UL) return;  
  lastTry = now;
  String cid = String("esp-relay-") + String(ESP.getChipId(), HEX);
  if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS, tOnline.c_str(), 0, true, "offline")) {
    mqtt.publish(tOnline.c_str(), "online", true);
    mqtt.subscribe(tCmd.c_str());
    lastLamp = -1;                      
  }
}

// ---------- hen gio ----------
uint32_t schedDay = 0xFFFFFFFF;         

bool inOnWindow(long sod) {              
  if (cfg.onSec == cfg.offSec) return false;
  if (cfg.onSec < cfg.offSec) return sod >= (long)cfg.onSec && sod < (long)cfg.offSec;  
  return sod >= (long)cfg.onSec || sod < (long)cfg.offSec;                              
}

void tickSchedule() {
  if (!cfg.schedEn) return;
  time_t n = time(nullptr);
  if (n < 1700000000) return;           
  long loc = (long)n + TZ_OFFSET;        
  static long last = 0;
  if (last == 0 || loc < last || loc - last > 30) {   
  
    long day = loc / 86400;
    if (schedDay != (uint32_t)day) {
      schedDay = (uint32_t)day;
      setLamp(inOnWindow(loc % 86400));
    }
    last = loc - 1;                     
  }
  for (long t = last + 1; t <= loc; t++) {           
    long sod = t % 86400;
    if (sod == (long)cfg.onSec)  setLamp(true);
    if (sod == (long)cfg.offSec) setLamp(false);
  }
  last = loc;
}

String fmtSec(uint32_t s) {
  char b[10];
  snprintf(b, sizeof(b), "%02u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60), (unsigned)(s % 60));
  return String(b);
}

bool parseHMS(const String& str, uint32_t& out) {   // "HH:MM" hoac "HH:MM:SS"
  int h = 0, m = 0, sec = 0;
  int k = sscanf(str.c_str(), "%d:%d:%d", &h, &m, &sec);
  if (k < 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return false;
  out = (uint32_t)h * 3600 + m * 60 + sec;
  return true;
}

// ---------- API ----------
void sendStatus() {
  time_t n = time(nullptr);
  bool ok = n > 1700000000;
  char tb[12] = "--:--:--";
  if (ok) {
    struct tm t;
    localtime_r(&n, &t);
    snprintf(tb, sizeof(tb), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  }
  long left = 0;
  if (!sleeping && cfg.idleMin > 0) {
    long l = (long)((unsigned long)cfg.idleMin * 60000UL - (millis() - lastAct)) / 1000;
    left = l > 0 ? l : 0;
  }
  bool ap = (WiFi.getMode() != WIFI_STA);

  String j;
  j.reserve(560);
  j += "{\"lamp\":";    j += lampIsOn() ? 1 : 0;
  j += ",\"sensed\":";  j += (sensed() >= 0) ? "true" : "false";
  j += ",\"pos\":";     j += coilOn ? 1 : 0;
  j += ",\"time\":\"";  j += tb; j += "\"";
  j += ",\"synced\":";  j += ok ? "true" : "false";
  j += ",\"epoch\":";   j += (unsigned long)n;
  j += ",\"se\":";      j += cfg.schedEn ? "true" : "false";
  j += ",\"on\":\"";    j += fmtSec(cfg.onSec); j += "\"";
  j += ",\"off\":\"";   j += fmtSec(cfg.offSec); j += "\"";
  {
    long nx = -1;                   
    int na = 0;                         
    if (ok && cfg.schedEn) {
      long sod = ((long)n + TZ_OFFSET) % 86400;
      long dOn  = ((long)cfg.onSec  - sod + 86400) % 86400;
      long dOff = ((long)cfg.offSec - sod + 86400) % 86400;
      if (dOn == 0)  dOn = 86400;
      if (dOff == 0) dOff = 86400;
      na = (dOn <= dOff) ? 1 : 0;
      nx = na ? dOn : dOff;
    }
    j += ",\"nx\":"; j += nx;
    j += ",\"na\":"; j += na;
  }
  j += ",\"idle\":";    j += cfg.idleMin;
  j += ",\"sleeping\":"; j += sleeping ? "true" : "false";
  j += ",\"left\":";    j += left;
  j += ",\"heap\":";    j += ESP.getFreeHeap();
  j += ",\"ram\":";     j += RAM_TOTAL;
  j += ",\"sketch\":";  j += ESP.getSketchSize();
  j += ",\"flash\":";   j += ESP.getFlashChipRealSize();
  j += ",\"vcc\":";     j += ESP.getVcc();
  j += ",\"rssi\":";    j += ap ? 0 : WiFi.RSSI();
  j += ",\"up\":";      j += (unsigned long)(millis() / 1000);
  j += ",\"cpu\":";     j += ESP.getCpuFreqMHz();
  j += ",\"ap\":";      j += ap ? "true" : "false";
  j += ",\"ip\":\"";    j += ap ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); j += "\"";
  long tl = 0;
  if (timerAt != 0) { long l = (long)(timerAt - millis()) / 1000; tl = l > 0 ? l : 0; }
  j += ",\"tleft\":"; j += tl;
  j += ",\"tset\":";  j += timerSet;
  j += ",\"mqttcfg\":"; j += mqttEnabled() ? "true" : "false";
  j += ",\"mqtt\":";    j += mqtt.connected() ? "true" : "false";
  j += "}";
  server.send(200, "application/json", j);
}

void hToggle() { touch(); toggleLamp(); delay(150); sendStatus(); }
void hOn()     { touch(); setLamp(true);  sendStatus(); }
void hOff()    { touch(); setLamp(false); sendStatus(); }
void hWake()   { touch(); sendStatus(); }

void hTimer() {                       
  touch();
  int m = server.arg("min").toInt();
  if (m <= 0) { timerAt = 0; timerSet = 0; sendStatus(); return; }
  if (m > 720) m = 720;
  bool on = (server.arg("set") != "off");
  setLamp(on);
  timerSet = on ? 1 : 2;
  timerAt = millis() + (unsigned long)m * 60000UL;
  if (timerAt == 0) timerAt = 1;
  sendStatus();
}

void hSetTime() {   
  touch();
  if (server.hasArg("t")) {
    unsigned long t = strtoul(server.arg("t").c_str(), nullptr, 10);
    if (t > 1700000000UL) {
      struct timeval tv;
      tv.tv_sec = (time_t)t;
      tv.tv_usec = 0;
      settimeofday(&tv, nullptr);
    }
  }
  sendStatus();
}

void hHist() {                          
  String j;
  j.reserve(HN * 10 + 50);
  j += "{\"step\":10,\"max\":"; j += HN;
  j += ",\"r\":[";
  for (uint8_t i = 0; i < hN; i++) { if (i) j += ','; j += (int)hR[(hHead + HN - hN + i) % HN]; }
  j += "],\"v\":[";
  for (uint8_t i = 0; i < hN; i++) { if (i) j += ','; j += (int)hV[(hHead + HN - hN + i) % HN]; }
  j += "]}";
  server.send(200, "application/json", j);
}

void hSched() {
  touch();
  uint32_t v;
  if (server.hasArg("en")) cfg.schedEn = server.arg("en").toInt() ? 1 : 0;
  if (server.hasArg("on")  && parseHMS(server.arg("on"), v))  cfg.onSec = v;
  if (server.hasArg("off") && parseHMS(server.arg("off"), v)) cfg.offSec = v;
  schedDay = 0xFFFFFFFF;                
  saveCfg();
  sendStatus();
}

void hCfg() {
  touch();
  if (server.hasArg("idle")) {
    int m = server.arg("idle").toInt();
    if (m < 0) m = 0;
    if (m > 120) m = 120;
    cfg.idleMin = m;
    saveCfg();
  }
  sendStatus();
}

void hFix() {                           
  touch();
  if (SENSE_PIN < 0 && server.hasArg("lamp")) {
    cfg.lampEst = server.arg("lamp").toInt() ? 1 : 0;
    saveCfg();
  }
  sendStatus();
}

// ---------- giao dien ----------
const char PAGE[] PROGMEM =
R"=====(<!DOCTYPE html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#050505">
<title>Đèn thông minh</title>
<style>
:root{--card:rgba(16,22,34,.72);--line:rgba(255,255,255,.08);--tx:#e8eef7;--mu:#8f9db3;--ok:#34d399;--bl:#3b82f6;--gold:#ffd43b}
*{box-sizing:border-box}
html,body{margin:0;min-height:100%}
body{background:#050505;color:var(--tx);font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif}

/* ===== Nền bầu trời đêm (Uiverse - kiranmayee-abbireddy) ===== */
.sky{position:fixed;inset:0;z-index:0;pointer-events:none}
.uiverse-midnight-sky{margin:0;padding:0;box-sizing:border-box;width:100%;height:100%;min-height:500px;overflow:hidden;background-color:#050505;position:relative;display:flex;align-items:center;justify-content:center}
.uiverse-midnight-sky .sky-canvas{width:100%;height:100%;position:absolute;inset:0;background:#050505}
.uiverse-midnight-sky .stars{position:absolute;inset:0;background-repeat:repeat;pointer-events:none}
.uiverse-midnight-sky .stars-1{background-image:radial-gradient(1px 1px at 10% 10%,#fff,transparent),radial-gradient(1px 1px at 30% 20%,#fff,transparent),radial-gradient(1px 1px at 50% 50%,#fff,transparent),radial-gradient(1px 1px at 70% 30%,#fff,transparent),radial-gradient(1px 1px at 90% 10%,#fff,transparent);background-size:200px 200px;animation:twinkle 3s ease-in-out infinite}
.uiverse-midnight-sky .stars-2{background-image:radial-gradient(1.5px 1.5px at 20% 40%,#fff,transparent),radial-gradient(1.5px 1.5px at 60% 85%,#fff,transparent),radial-gradient(1.5px 1.5px at 85% 65%,#fff,transparent);background-size:300px 300px;animation:twinkle 5s ease-in-out infinite 1s}
.uiverse-midnight-sky .stars-3{background-image:radial-gradient(2px 2px at 40% 70%,#fff,transparent),radial-gradient(2px 2px at 10% 80%,#fff,transparent),radial-gradient(2px 2px at 80% 40%,#fff,transparent);background-size:400px 400px;animation:twinkle 7s ease-in-out infinite 2s}
.uiverse-midnight-sky .meteor{position:absolute;width:2px;height:2px;background:#fff;border-radius:50%;box-shadow:0 0 10px 2px rgba(255,255,255,.5);opacity:0;pointer-events:none}
.uiverse-midnight-sky .meteor::after{content:"";position:absolute;top:50%;transform:translateY(-50%);width:80px;height:1px;background:linear-gradient(90deg,#fff,transparent)}
.uiverse-midnight-sky .m1{top:10%;left:110%;animation:shoot 8s linear infinite}
.uiverse-midnight-sky .m2{top:30%;left:110%;animation:shoot 12s linear infinite 4s}
.uiverse-midnight-sky .m3{top:50%;left:110%;animation:shoot 10s linear infinite 2s}
.uiverse-midnight-sky .moon{position:absolute;top:15%;right:15%;width:80px;height:80px;border-radius:50%;background:transparent;box-shadow:15px 15px 0 0 #fdfbd3;filter:drop-shadow(0 0 15px rgba(253,251,211,.4));z-index:10}
@keyframes twinkle{0%,100%{opacity:1}50%{opacity:.2}}
@keyframes shoot{0%{transform:translateX(0) translateY(0) rotate(-35deg);opacity:0}5%{opacity:1}15%{transform:translateX(-1500px) translateY(1000px) rotate(-35deg);opacity:0}100%{transform:translateX(-1500px) translateY(1000px) rotate(-35deg);opacity:0}}
@media (prefers-reduced-motion:reduce){.uiverse-midnight-sky *{animation:none!important}}

/* ===== Công tắc ngày/đêm (Uiverse - Galahhad) ===== */
.theme-switch{--toggle-size:30px;--container-width:5.625em;--container-height:2.5em;--container-radius:6.25em;--container-light-bg:#3D7EAE;--container-night-bg:#1D1F2C;--circle-container-diameter:3.375em;--sun-moon-diameter:2.125em;--sun-bg:#ECCA2F;--moon-bg:#C4C9D1;--spot-color:#959DB1;--circle-container-offset:calc((var(--circle-container-diameter) - var(--container-height)) / 2 * -1);--stars-color:#fff;--clouds-color:#F3FDFF;--back-clouds-color:#AACADF;--transition:.5s cubic-bezier(0,-0.02,0.4,1.25);--circle-transition:.3s cubic-bezier(0,-0.02,0.35,1.17);display:inline-block}
.theme-switch,.theme-switch *,.theme-switch *::before,.theme-switch *::after{box-sizing:border-box;margin:0;padding:0;font-size:var(--toggle-size)}
.hero .theme-switch{--toggle-size:44px}
.theme-switch__container{width:var(--container-width);height:var(--container-height);background-color:var(--container-light-bg);border-radius:var(--container-radius);overflow:hidden;cursor:pointer;box-shadow:0em -0.062em 0.062em rgba(0,0,0,.25),0em 0.062em 0.125em rgba(255,255,255,.94);transition:var(--transition);position:relative}
.theme-switch__container::before{content:"";position:absolute;z-index:1;inset:0;box-shadow:0em 0.05em 0.187em rgba(0,0,0,.25) inset,0em 0.05em 0.187em rgba(0,0,0,.25) inset;border-radius:var(--container-radius)}
.theme-switch__checkbox{display:none}
.theme-switch__circle-container{width:var(--circle-container-diameter);height:var(--circle-container-diameter);background-color:rgba(255,255,255,.1);position:absolute;left:var(--circle-container-offset);top:var(--circle-container-offset);border-radius:var(--container-radius);box-shadow:inset 0 0 0 3.375em rgba(255,255,255,.1),inset 0 0 0 3.375em rgba(255,255,255,.1),0 0 0 0.625em rgba(255,255,255,.1),0 0 0 1.25em rgba(255,255,255,.1);display:flex;transition:var(--circle-transition);pointer-events:none}
.theme-switch__sun-moon-container{pointer-events:auto;position:relative;z-index:2;width:var(--sun-moon-diameter);height:var(--sun-moon-diameter);margin:auto;border-radius:var(--container-radius);background-color:var(--sun-bg);box-shadow:0.062em 0.062em 0.062em 0em rgba(254,255,239,.61) inset,0em -0.062em 0.062em 0em #a1872a inset;filter:drop-shadow(0.062em 0.125em 0.125em rgba(0,0,0,.25)) drop-shadow(0em 0.062em 0.125em rgba(0,0,0,.25));overflow:hidden;transition:var(--transition)}
.theme-switch__moon{transform:translateX(100%);width:100%;height:100%;background-color:var(--moon-bg);border-radius:inherit;box-shadow:0.062em 0.062em 0.062em 0em rgba(254,255,239,.61) inset,0em -0.062em 0.062em 0em #969696 inset;transition:var(--transition);position:relative}
.theme-switch__spot{position:absolute;top:0.75em;left:0.312em;width:0.75em;height:0.75em;border-radius:var(--container-radius);background-color:var(--spot-color);box-shadow:0em 0.0312em 0.062em rgba(0,0,0,.25) inset}
.theme-switch__spot:nth-of-type(2){width:0.375em;height:0.375em;top:0.937em;left:1.375em}
)====="
R"=====(.theme-switch__spot:nth-last-of-type(3){width:0.25em;height:0.25em;top:0.312em;left:0.812em}
.theme-switch__clouds{width:1.25em;height:1.25em;background-color:var(--clouds-color);border-radius:var(--container-radius);position:absolute;bottom:-0.625em;left:0.312em;box-shadow:0.937em 0.312em var(--clouds-color),-0.312em -0.312em var(--back-clouds-color),1.437em 0.375em var(--clouds-color),0.5em -0.125em var(--back-clouds-color),2.187em 0 var(--clouds-color),1.25em -0.062em var(--back-clouds-color),2.937em 0.312em var(--clouds-color),2em -0.312em var(--back-clouds-color),3.625em -0.062em var(--clouds-color),2.625em 0em var(--back-clouds-color),4.5em -0.312em var(--clouds-color),3.375em -0.437em var(--back-clouds-color),4.625em -1.75em 0 0.437em var(--clouds-color),4em -0.625em var(--back-clouds-color),4.125em -2.125em 0 0.437em var(--back-clouds-color);transition:0.5s cubic-bezier(0,-0.02,0.4,1.25)}
.theme-switch__stars-container{position:absolute;color:var(--stars-color);top:-100%;left:0.312em;width:2.75em;height:auto;transition:var(--transition)}
.theme-switch__stars-container svg{display:block;width:100%;height:auto}
.theme-switch__checkbox:checked+.theme-switch__container{background-color:var(--container-night-bg)}
.theme-switch__checkbox:checked+.theme-switch__container .theme-switch__circle-container{left:calc(100% - var(--circle-container-offset) - var(--circle-container-diameter))}
.theme-switch__checkbox:checked+.theme-switch__container .theme-switch__circle-container:hover{left:calc(100% - var(--circle-container-offset) - var(--circle-container-diameter) - 0.187em)}
.theme-switch__circle-container:hover{left:calc(var(--circle-container-offset) + 0.187em)}
.theme-switch__checkbox:checked+.theme-switch__container .theme-switch__moon{transform:translate(0)}
.theme-switch__checkbox:checked+.theme-switch__container .theme-switch__clouds{bottom:-4.062em}
.theme-switch__checkbox:checked+.theme-switch__container .theme-switch__stars-container{top:50%;transform:translateY(-50%)}

/* ===== Ô nhập giờ (Uiverse - Smit-Prajapati / coslac) ===== */
.input-container{display:flex;border-radius:1rem;background:linear-gradient(173deg,#23272f 0%,#14161a 100%);box-shadow:10px 10px 20px #0e1013,-10px -10px 40px #383e4b;padding:.3rem;gap:.3rem}
.input-container input{border-radius:.8rem;background:#23272f;box-shadow:inset 5px 5px 10px #0e1013,inset -5px -5px 10px #383e4b,0px 0px 100px rgba(255,212,59,0),0px 0px 100px rgba(255,102,0,0);width:100%;flex-basis:100%;min-width:0;padding:1rem;border:1px solid transparent;color:#fff;font-size:1.1rem;text-align:center;color-scheme:dark;transition:all .2s ease-in-out}
.input-container input:focus{border:1px solid #ffd43b;outline:none;box-shadow:inset 0px 0px 10px rgba(255,102,0,.5),inset 0px 0px 10px rgba(255,212,59,.5),0px 0px 60px rgba(255,212,59,.5),0px 0px 60px rgba(255,102,0,.5)}
@media (max-width:500px){.input-container.stack{flex-direction:column}}

/* ===== Bố cục ===== */
.wrap{position:relative;z-index:1;max-width:440px;margin:0 auto;padding:18px max(16px,env(safe-area-inset-right)) 40px max(16px,env(safe-area-inset-left))}
.cols,.col{display:block}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}
h1{font-size:19px;font-weight:600;margin:0}
h2{font-size:13px;font-weight:600;letter-spacing:.06em;text-transform:uppercase;color:var(--mu);margin:0 0 12px}
.chip{font-size:12px;padding:5px 10px;border-radius:99px;background:rgba(255,255,255,.08);color:var(--mu)}
.chip.on{background:rgba(52,211,153,.18);color:var(--ok)}
.card{background:var(--card);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);border:1px solid var(--line);border-radius:18px;padding:16px;margin-bottom:12px}
.hero{text-align:center;padding:30px 16px 22px}
#lamp{font-size:26px;font-weight:700;margin-top:22px}
#note,#tim{font-size:13px;color:var(--mu);margin-top:4px;min-height:18px}
#tim{color:var(--gold)}
.row{display:flex;gap:10px;margin-bottom:8px}
.row label{flex:1;font-size:13px;color:var(--mu);text-align:center}
select{width:100%;margin-top:6px;padding:11px 10px;font-size:16px;border-radius:12px;border:1px solid var(--line);background:#0e1522;color:var(--tx);color-scheme:dark}
.btn{width:100%;margin-top:12px;padding:12px;font-size:15px;font-weight:600;border:0;border-radius:12px;background:var(--bl);color:#fff;cursor:pointer}
.btn.g{background:#223049}
.btn.act{background:var(--gold);color:#111}
.half{display:flex;gap:10px}
.half .btn{margin-top:0}
.top{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px;font-size:15px}
.sw{position:relative;width:46px;height:26px}
)====="
R"=====(.sw input{opacity:0;width:0;height:0}
.sw i{position:absolute;inset:0;background:#2a3850;border-radius:99px;transition:.2s}
.sw i:before{content:"";position:absolute;width:20px;height:20px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}
.sw input:checked+i{background:var(--ok)}
.sw input:checked+i:before{transform:translateX(20px)}
.mu{color:var(--mu);font-size:13px;margin-top:10px}
.bar{height:8px;border-radius:99px;background:rgba(255,255,255,.07);overflow:hidden;margin:6px 0 12px}
.bar i{display:block;height:100%;width:0;background:linear-gradient(90deg,#3b82f6,#34d399);transition:.2s}
.kv{display:flex;justify-content:space-between;font-size:14px;margin-bottom:2px}
.kv span:first-child{color:var(--mu)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px 16px;margin-top:6px;font-size:14px}
.grid div span{display:block;color:var(--mu);font-size:12px}
.note2{font-size:12.5px;line-height:1.5;margin-top:10px;padding:9px 11px;border-radius:10px;background:rgba(255,255,255,.06);color:var(--mu)}
.note2.ok{background:rgba(52,211,153,.14);color:var(--ok)}
.note2.warn{background:rgba(255,212,59,.14);color:var(--gold)}
canvas.ch{width:100%;height:170px;display:block;touch-action:pan-y;cursor:crosshair}
.lnk{margin-top:8px;background:none;border:1px solid var(--line);color:var(--mu);font-size:12px;padding:6px 12px;border-radius:99px;cursor:pointer}
.lnk:hover{color:var(--tx);border-color:rgba(255,255,255,.25)}
.uiverse-midnight-sky{min-height:0}
.btn{transition:filter .15s,transform .1s}
.btn:active{transform:scale(.98)}
@media (hover:hover){.btn:hover{filter:brightness(1.15)}.input-container input:hover{border-color:rgba(255,212,59,.35)}}
/* ===== Responsive: tablet / điện thoại nằm ngang / PC ===== */
@media (min-width:640px){
  .wrap{max-width:980px;padding-top:24px}
  h1{font-size:22px}
  .cols{display:grid;grid-template-columns:1fr 1fr;gap:0 14px;align-items:start}
  .col:nth-child(3){grid-column:1/-1;display:grid;grid-template-columns:1fr 1fr;gap:0 14px;align-items:start}
}
@media (min-width:1000px){
  .wrap{max-width:1280px;padding-top:28px;padding-bottom:48px}
  .cols{grid-template-columns:repeat(3,1fr);gap:0 18px}
  .col:nth-child(3){grid-column:auto;display:block}
  .card{padding:20px;margin-bottom:16px}
  .hero{padding:36px 20px 28px}
  .hero .theme-switch{--toggle-size:52px}
  #lamp{font-size:30px}
}
@media (min-width:1600px){
  .wrap{max-width:1500px}
  .cols{gap:0 24px}
}
/* điện thoại nằm ngang (màn hình thấp): thu gọn khoảng trống */
@media (orientation:landscape) and (max-height:520px){
  .wrap{padding-top:10px;padding-bottom:20px}
  header{margin-bottom:10px}
  .card{padding:12px;margin-bottom:10px}
  .hero{padding:18px 12px 14px}
  .hero .theme-switch{--toggle-size:36px}
  #lamp{margin-top:14px;font-size:22px}
  .moon{width:50px;height:50px;box-shadow:10px 10px 0 0 #fdfbd3}
}
</style></head><body>
<div class="sky"><div class="uiverse-midnight-sky"><div class="sky-canvas">
  <div class="stars stars-1"></div><div class="stars stars-2"></div><div class="stars stars-3"></div>
  <div class="meteor m1"></div><div class="meteor m2"></div><div class="meteor m3"></div>
  <div class="moon"></div>
</div></div></div>

<div class="wrap">
<header><h1>💡 Đèn thông minh</h1><span id="chip" class="chip">...</span></header>

<div class="cols"><div class="col">
<section class="card hero">
  <label class="theme-switch" aria-label="Bật tắt đèn">
    <input type="checkbox" class="theme-switch__checkbox" id="sw" onchange="sw()">
    <div class="theme-switch__container">
      <div class="theme-switch__clouds"></div>
      <div class="theme-switch__stars-container"><svg viewBox="0 0 144 55" fill="currentColor" xmlns="http://www.w3.org/2000/svg"><path d="M20 5Q20 12 27 12Q20 12 20 19Q20 12 13 12Q20 12 20 5ZM48 36Q48 40 52 40Q48 40 48 44Q48 40 44 40Q48 40 48 36ZM80 6Q80 14 88 14Q80 14 80 22Q80 14 72 14Q80 14 80 6ZM112 33Q112 38 117 38Q112 38 112 43Q112 38 107 38Q112 38 112 33ZM131 7Q131 12 136 12Q131 12 131 17Q131 12 126 12Q131 12 131 7ZM66 41Q66 44 69 44Q66 44 66 47Q66 44 63 44Q66 44 66 41ZM100 21Q100 24 103 24Q100 24 100 27Q100 24 97 24Q100 24 100 21ZM34 29Q34 32 37 32Q34 32 34 35Q34 32 31 32Q34 32 34 29ZM8 37Q8 40 11 40Q8 40 8 43Q8 40 5 40Q8 40 8 37Z"/></svg></div>
      <div class="theme-switch__circle-container">
        <div class="theme-switch__sun-moon-container">
          <div class="theme-switch__moon">
            <div class="theme-switch__spot"></div><div class="theme-switch__spot"></div><div class="theme-switch__spot"></div>
          </div>
        </div>
      </div>
    </div>
  </label>
)====="
R"=====(  <div id="lamp">...</div>
  <div id="note"></div>
  <button class="lnk" id="fixb" onclick="fixLamp()" style="display:none">Đèn thật đang ngược với hiển thị? Bấm để đảo</button>
  <div id="tim"></div>
</section>

<section class="card">
  <h2>Hẹn giờ hằng ngày (từ giờ đến giờ)</h2>
  <div class="top"><span>Bật hẹn giờ</span><label class="sw"><input type="checkbox" id="se" oninput="dirty=1;schedNote(lastD)"><i></i></label></div>
  <div class="row"><label>Bật lúc</label><label>Tắt lúc</label></div>
  <div class="input-container"><input type="time" step="1" id="ton" oninput="dirty=1;schedNote(lastD)"><input type="time" step="1" id="toff" oninput="dirty=1;schedNote(lastD)"></div>
  <button class="btn" onclick="saveS()">Lưu hẹn giờ</button>
  <div class="note2" id="sn">...</div>
  <div class="mu" id="clock"></div>
  <button class="btn g" onclick="setTime()">Đồng bộ giờ từ điện thoại</button>
</section>

</div><div class="col">
<section class="card">
  <h2>Đếm giờ một lần</h2>
  <div class="row"><label>Số phút</label></div>
  <div class="input-container"><input type="number" id="tmin" min="1" max="720" value="30" inputmode="numeric"></div>
  <div class="half" style="margin-top:12px"><button class="btn g" onclick="setTimer('on')">Bật rồi tự tắt</button><button class="btn g" onclick="setTimer('off')">Tắt rồi tự bật</button></div>
  <button class="btn g" onclick="api('timer?min=0')">Hủy đếm giờ</button>
  <div class="note2" id="tn">Chưa đặt đếm giờ</div>
</section>

<section class="card">
  <h2>Đồ thị mạng WiFi (RSSI)</h2>
  <canvas class="ch" id="cr"></canvas>
  <div class="mu" id="ri">Đang lấy dữ liệu...</div>
</section>

<section class="card">
  <h2>Đồ thị điện (Vcc nguồn)</h2>
  <canvas class="ch" id="cv"></canvas>
  <div class="mu" id="vi">Đang lấy dữ liệu...</div>
</section>

</div><div class="col">
<section class="card">
  <h2>Chế độ ngủ</h2>
)====="
R"=====(  <label class="mu" style="display:block;margin:0">Không thao tác thì ngủ sau
    <select id="idle" onchange="saveI()">
      <option value="0">Không ngủ</option><option value="1">1 phút</option><option value="5">5 phút</option>
      <option value="15">15 phút</option><option value="30">30 phút</option><option value="60">1 giờ</option>
    </select></label>
  <div class="mu" id="slp"></div>
  <button class="btn g" onclick="api('wake')">Đánh thức</button>
</section>

<section class="card">
  <h2>Hệ thống</h2>
  <div class="kv"><span>RAM</span><span id="ramt">-</span></div><div class="bar"><i id="ramb"></i></div>
  <div class="kv"><span>ROM (chương trình / flash)</span><span id="romt">-</span></div><div class="bar"><i id="romb"></i></div>
  <div class="grid">
    <div><span>Điện áp nguồn (Vcc)</span><b id="vcc">-</b></div>
    <div><span>WiFi</span><b id="rssi">-</b></div>
    <div><span>Chạy được</span><b id="up">-</b></div>
    <div><span>CPU</span><b id="cpu">-</b></div>
    <div><span>IP</span><b id="ip">-</b></div>
    <div><span>Điều khiển từ xa</span><b id="mq">-</b></div>
    <div><span>Nhiệt độ chip</span><b>ESP8266 không có</b></div>
  </div>
</section>
</div></div>
</div>

<script>
var dirty=0,autoTried=0,lastD=null;
function hmt(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=s%60,t='';if(h)t+=h+' giờ ';if(h||m)t+=m+' phút ';return t+x+' giây'}
function schedNote(d){
  var e=$('sn');if(!d)return;
  if(dirty){e.textContent='⚠ Bạn đã sửa nhưng CHƯA bấm "Lưu hẹn giờ"';e.className='note2 warn';return}
  if(!d.se){e.textContent='Hẹn giờ đang TẮT (chưa có lịch nào chạy)';e.className='note2';return}
  var t='✔ Đã đặt: bật lúc '+d.on+' → tắt lúc '+d.off+' (lặp mỗi ngày)';
  if(!d.synced)t+=' · đang chờ lấy giờ nên chưa chạy';
  else if(d.nx>0)t+=' · '+(d.na?'bật':'tắt')+' kế tiếp sau '+hmt(d.nx);
  e.textContent=t;e.className='note2 ok';
}
var PL=46,PR=12,PT=12,PB=22;
function ago(sec){return sec<90?Math.round(sec)+' giây':Math.round(sec/60)+' phút'}
function draw(id,a,col,fmt,unit,span,step){var c=$(id);c._d={a:a,col:col,fmt:fmt,unit:unit,span:span,step:step};render(c)}
function render(c){
  var D=c._d;if(!D)return;
  var a=D.a,n=a.length,r=window.devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;
  c.width=w*r;c.height=h*r;
  var x=c.getContext('2d');x.setTransform(r,0,0,r,0,0);x.clearRect(0,0,w,h);
  x.font='11px system-ui,sans-serif';
  var vals=a.filter(function(q){return q!==null});
  if(!vals.length){x.fillStyle='#8f9db3';x.textAlign='center';x.fillText('Chưa có dữ liệu',w/2,h/2);return}
  var pw=w-PL-PR,ph=h-PT-PB;
  var lo=Math.min.apply(null,vals),hi=Math.max.apply(null,vals);
  if(hi-lo<D.span){var m=(hi+lo)/2;lo=m-D.span/2;hi=m+D.span/2}
  function px(i){return n>1?PL+pw*i/(n-1):PL+pw/2}
  function py(v){return PT+ph*(1-(v-lo)/(hi-lo))}
  x.textAlign='right';x.textBaseline='middle';
  for(var i=0;i<=3;i++){
    var y=PT+ph*i/3;
    x.strokeStyle='rgba(255,255,255,.07)';x.lineWidth=1;x.beginPath();x.moveTo(PL,y);x.lineTo(w-PR,y);x.stroke();
    x.fillStyle='#8f9db3';x.fillText(D.fmt(hi-(hi-lo)*i/3),PL-6,y);
  }
  x.textBaseline='alphabetic';x.textAlign='left';x.fillStyle='#8f9db3';
  if(n>1)x.fillText('-'+ago((n-1)*D.step),PL,h-6);
  x.textAlign='right';x.fillText('bây giờ',w-PR,h-6);
  var g=x.createLinearGradient(0,PT,0,PT+ph);g.addColorStop(0,D.col+'66');g.addColorStop(1,D.col+'00');
  var seg=[];
  for(i=0;i<=n;i++){
    if(i<n&&a[i]!==null){seg.push(i)}
    else if(seg.length){
      if(seg.length===1){x.fillStyle=D.col;x.beginPath();x.arc(px(seg[0]),py(a[seg[0]]),2.5,0,7);x.fill()}
      else{
        x.beginPath();
        seg.forEach(function(k,j){if(j)x.lineTo(px(k),py(a[k]));else x.moveTo(px(k),py(a[k]))});
        x.strokeStyle=D.col;x.lineWidth=2;x.lineJoin='round';x.stroke();
        x.lineTo(px(seg[seg.length-1]),PT+ph);x.lineTo(px(seg[0]),PT+ph);x.closePath();x.fillStyle=g;x.fill();
      }
      seg=[];
    }
  }
  var hv=(c._h>=0&&c._h<n&&a[c._h]!==null)?c._h:-1;
  var k=hv>=0?hv:-1;
  if(k<0){for(i=n-1;i>=0;i--){if(a[i]!==null){k=i;break}}}
  var X=px(k),Y=py(a[k]);
  if(hv>=0){x.strokeStyle='rgba(255,255,255,.3)';x.setLineDash([3,3]);x.beginPath();x.moveTo(X,PT);x.lineTo(X,PT+ph);x.stroke();x.setLineDash([])}
  x.fillStyle=D.col;x.beginPath();x.arc(X,Y,4,0,7);x.fill();
  x.strokeStyle='#0b1019';x.lineWidth=2;x.stroke();
  if(hv>=0){
    var t=D.fmt(a[k])+D.unit+' · '+(k===n-1?'bây giờ':ago((n-1-k)*D.step)+' trước'),tw=x.measureText(t).width+14;
    var bx=Math.max(PL,Math.min(w-PR-tw,X-tw/2)),by=PT+2;
    x.fillStyle='rgba(8,12,20,.92)';x.fillRect(bx,by,tw,20);
    x.fillStyle='#e8eef7';x.textAlign='left';x.textBaseline='middle';x.fillText(t,bx+7,by+10);
  }
}
['cr','cv'].forEach(function(id){
  var c=$(id);
  function mv(e){var D=c._d;if(!D)return;var b=c.getBoundingClientRect(),n=D.a.length,pw=b.width-PL-PR;
    c._h=n>1?Math.max(0,Math.min(n-1,Math.round((e.clientX-b.left-PL)/pw*(n-1)))):0;render(c)}
  c.addEventListener('pointermove',mv);c.addEventListener('pointerdown',mv);
  c.addEventListener('pointerleave',function(){c._h=-1;render(c)});
});
function hist(){
  fetch('/hist').then(function(r){return r.json()}).then(function(d){
    var r=d.r.map(function(v){return v===0?null:v}),v=d.v.map(function(m){return m/1000});
    var rv=r.filter(function(q){return q!==null}),span=ago(d.r.length*d.step);
    draw('cr',r,'#3b82f6',function(q){return Math.round(q)+''},' dBm',4,d.step);
    if(rv.length){$('ri').textContent='Hiện tại '+rv[rv.length-1]+' dBm · thấp nhất '+Math.min.apply(null,rv)+' · cao nhất '+Math.max.apply(null,rv)+' ('+span+' gần nhất)'}
    else $('ri').textContent='Chưa có dữ liệu WiFi (đang ở chế độ phát WiFi?)';
    draw('cv',v,'#ffd43b',function(q){return q.toFixed(2)},' V',0.04,d.step);
    if(v.length){$('vi').textContent='Hiện tại '+v[v.length-1].toFixed(2)+' V · thấp nhất '+Math.min.apply(null,v).toFixed(2)+' · cao nhất '+Math.max.apply(null,v).toFixed(2)+' V ('+span+' gần nhất)'}
  }).catch(function(){});
}
function $(i){return document.getElementById(i)}
function kb(n){return (n/1024).toFixed(0)+' KB'}
function dur(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h>0?h+' giờ '+m+' phút':m+' phút '+(s%60)+' giây'}
function mmss(s){return Math.floor(s/60)+':'+('0'+s%60).slice(-2)}
function show(d){
  $('sw').checked=!d.lamp;   // den sang: canh ban ngay (mat troi); den tat: canh dem (trang sao)
  $('lamp').textContent=d.lamp?'Đèn sáng':'Đèn tắt';
  $('lamp').style.color=d.lamp?'#ffd43b':'#8f9db3';
  $('note').textContent=d.sensed?'Đo từ cảm biến':'Ước tính (chưa có cảm biến)';
  $('fixb').style.display=d.sensed?'none':'inline-block';
  $('tim').textContent=d.tleft>0?('Đếm giờ: tự '+(d.tset==1?'tắt':'bật')+' sau '+mmss(d.tleft)):'';
  var tn=$('tn');
  if(d.tleft>0){tn.textContent='✔ Đã đặt đếm giờ: đèn sẽ tự '+(d.tset==1?'tắt':'bật')+' sau '+hmt(d.tleft);tn.className='note2 ok'}
  else{tn.textContent='Chưa đặt đếm giờ';tn.className='note2'}
  lastD=d;schedNote(d);
  var c=$('chip');c.textContent=d.sleeping?'Đang ngủ':'Đang thức';c.className='chip'+(d.sleeping?'':' on');
  if(!dirty){$('se').checked=d.se;$('ton').value=d.on;$('toff').value=d.off;}
)====="
R"=====(  if(document.activeElement!==$('idle'))$('idle').value=d.idle;
  $('clock').textContent=d.synced?('Giờ trên thiết bị: '+d.time):'Chưa có giờ nên hẹn giờ chưa chạy. Đang lấy giờ từ điện thoại...';
  if(d.synced)autoTried=0;else if(!autoTried){autoTried=1;setTime();}
  $('slp').textContent=d.idle==0?'Chế độ ngủ đang tắt':(d.sleeping?'Đang ngủ nhẹ để tiết kiệm điện, bấm bất kỳ nút nào sẽ tỉnh':'Còn '+mmss(d.left)+' nữa sẽ ngủ');
  var ru=d.ram-d.heap;
  $('ramt').textContent=kb(ru)+' / '+kb(d.ram);$('ramb').style.width=(ru/d.ram*100)+'%';
  $('romt').textContent=kb(d.sketch)+' / '+(d.flash/1048576).toFixed(0)+' MB';$('romb').style.width=(d.sketch/d.flash*100)+'%';
  $('vcc').textContent=(d.vcc/1000).toFixed(2)+' V';
  $('rssi').textContent=d.ap?'Chế độ phát WiFi':d.rssi+' dBm';
  $('mq').textContent=!d.mqttcfg?'Chưa cấu hình':(d.mqtt?'Đã kết nối':'Mất kết nối');
  $('up').textContent=dur(d.up);$('cpu').textContent=d.cpu+' MHz';$('ip').textContent=d.ip;
}
var seq=0,applied=0,pend=0;
function api(p){var s=++seq;pend++;
  return fetch('/'+p).then(function(r){return r.json()}).then(function(d){pend--;if(s>applied){applied=s;show(d)}}).catch(function(){pend--})}
function sw(){var want=!$('sw').checked;$('lamp').textContent=want?'Đang bật...':'Đang tắt...';api(want?'on':'off')}
function fixLamp(){if(lastD&&confirm('Chỉ đảo trạng thái hiển thị cho khớp đèn thật (không đóng/ngắt relay). Tiếp tục?'))api('fix?lamp='+(lastD.lamp?0:1))}
function setTime(){api('settime?t='+Math.floor(Date.now()/1000))}
function saveS(){dirty=0;api('sched?en='+($('se').checked?1:0)+'&on='+$('ton').value+'&off='+$('toff').value)}
function saveI(){api('cfg?idle='+$('idle').value)}
function setTimer(s){var m=parseInt($('tmin').value)||0;if(m<1)return;api('timer?set='+s+'&min='+m)}
setInterval(function(){if(!pend)api('status')},3000);
setInterval(hist,10000);
window.addEventListener('resize',hist);
api('status');hist();

</script></body></html>
)=====";

// ---------- setup / loop ----------
void setup() {
  EEPROM.begin(32);
  loadCfg();
  pinMode(RELAY_PIN, OUTPUT);
  driveCoil(cfg.coil == 1);              // khoi phuc vi tri relay ngay luc khoi dong
  cfg.lampEst = cfg.coil;                // dong bo uoc tinh voi relay sau khi mat dien/khoi dong lai
  if (SENSE_PIN >= 0) pinMode(SENSE_PIN, INPUT_PULLUP);
  if (BTN_PIN >= 0)   pinMode(BTN_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setOutputPower(14);               // giam cong suat phat cho bot nong
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) delay(300);

  Serial.begin(115200);
  configTime(TZ_OFFSET, 0, "pool.ntp.org", "time.google.com");   // dat mui gio; NTP chi chay khi co internet
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nIP: ");
    Serial.println(WiFi.localIP());
  } else {
    startAP();
    Serial.println("\nKhong vao duoc WiFi. AP: ESP-Relay / 12345678 -> http://192.168.4.1");
  }

  MDNS.begin(HOSTNAME);
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.begin();
  mqttSetup();

  server.on("/", []() { touch(); server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/status", sendStatus);      
  server.on("/toggle", hToggle);
  server.on("/on", hOn);
  server.on("/off", hOff);
  server.on("/wake", hWake);
  server.on("/settime", hSetTime);
  server.on("/timer", hTimer);
  server.on("/hist", hHist);            
  server.on("/sched", hSched);
  server.on("/cfg", hCfg);
  server.on("/fix", hFix);
  server.begin();

  lastAct = millis();
  setAwake();
  histPush();
}

void loop() {
  ArduinoOTA.handle();
  MDNS.update();
  server.handleClient();
  mqttLoop();

  if (BTN_PIN >= 0 && digitalRead(BTN_PIN) == LOW) touch();

  static unsigned long lastTick = 0;
  unsigned long now = millis();
  if (now - lastTick >= 1000) {
    lastTick = now;
    tickSchedule();
    static uint8_t hc = 0;
    if (++hc >= 10) { hc = 0; histPush(); } 
    if (timerAt != 0 && (long)(millis() - timerAt) >= 0) {   
      setLamp(timerSet == 2);
      timerAt = 0;
      timerSet = 0;
    }
  }

  if (cfg.idleMin == 0) {
    if (sleeping) setAwake();
  } else if (!sleeping && now - lastAct > (unsigned long)cfg.idleMin * 60000UL) {
    setSleepy();
  }

  static unsigned long lastCheck = 0;
  if (now - lastCheck > 10000) {
    lastCheck = now;
    if (apFallback) {
      if (WiFi.status() == WL_CONNECTED) { 
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        apFallback = false;
        setAwake();
      }
    } else if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }
  }

  delay(sleeping ? 20 : 2);             
}