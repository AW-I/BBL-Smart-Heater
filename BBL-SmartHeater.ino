/*
 * Bambu Smart Heater V1.0 - ESP8266
 * ─────────────────────────────────────────────────────
 * Libraries required:
 *   - ArduinoJson   (V7.4.3)
 *   - WiFiManager   (V2.0.17)
 *   - PubSubClient  (V2.8)
 *
 * Board: ESP8266 Relay Module - (https://www.aliexpress.com/item/1005010435047645.html)
 * Flash: 1MB Minimum
 * ─────────────────────────────────────────────────────
 */

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <EEPROM.h>

//Convert to string
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

// ── Hardware Configuration──────────────────────────
#define RELAY_PIN 4
#define RELAY_ON 1
#define RELAY_OFF 0

#define FAN_PIN         0       // GPIO0
#define FAN_FREQ        25000   // 25 kHz – 4-wire fan spec
#define PWM_RANGE       1023    // ESP8266 default (0–1023)
#define FAN_FULL        1023    // 100% duty cycle
#define FAN_OFF_DUTY    0       // 0% – most 4-wire fans stop completely

// ── Configuration ──────────────────────────────────
#define DATA_TIMEOUT_MS       60000UL // Watchdog timeout for receiving data.
#define MAX_BED_TEMP        120.0  // Sanity for max bed temp

// Config portal AP name / password
#define PORTAL_SSID    "BambuSmartHeaterV1"
#define PORTAL_PASS    NULL

// MQTT
#define MQTT_PORT           8883
#define MQTT_USER           "bblp"
#define MQTT_MAX_SIZE       24576

// How often to keep-alive / reconnect (ms)
#define RECONNECT_MS          10000
#define WIFI_MANAGER_TIMEOUT  120 // Timeout before rebooting from wifi manager
#define WIFI_CONNECT_TIMEOUT  30
// ──────────────────────────────────────────────────────

// ── EEPROM layout for runtime config ──────────────────
#define EEPROM_SIZE    256
#define ADDR_MAGIC     0           // 1 byte: 0xAB = valid
#define MAGIC_BYTE     0xAB

// ── Defaults for config ──────────────────
#define DEFAULT_FAN_MAX_PWM 100
#define DEFAULT_FAN_MIN_PWM 0
#define DEFAULT_HEATER_ON_BEDTEMP   90.0  // Bed setpoint to enable the heater
#define DEFAULT_FAN_ON_CHAMBER_TEMP  60   // Chamber setpoint to enable the fan.
#define DEFAULT_CHAMBER_TRIP_TEMP   70.0   // Chamber too hot safety latch
#define CHAMBER_RESET_TEMP_OFFSET  10.0   // Chamber cool enough safety latch

#define BED_TEMP_MAGIC_PORTAL 23

struct __attribute__((packed)) Config {
  char ip[16];
  char code[9];
  int heaterOnTemp;
  int fanOnTemp;
  int chamberMaxTemp;
  int fanSpeedMax;
  int fanSpeedMin;
};

// Bambu Config
Config cfg = {
  "",  // ip
  "",  // code
  DEFAULT_HEATER_ON_BEDTEMP,  // heaterOnTemp
  DEFAULT_FAN_ON_CHAMBER_TEMP,     // fanOnTemp
  DEFAULT_CHAMBER_TRIP_TEMP,  // chamberMaxTemp
  DEFAULT_FAN_MAX_PWM,        // fanSpeedMax
  DEFAULT_FAN_MIN_PWM         // fanSpeedMin
};

// ── WiFiManager custom parameters ─────────────────────
WiFiManagerParameter *wm_ip;
WiFiManagerParameter *wm_code;
WiFiManagerParameter *wm_bed_heater_temp;
WiFiManagerParameter *wm_chamber_fan_temp;
WiFiManagerParameter *wm_pwm_max;
WiFiManagerParameter *wm_pwm_min;
WiFiManagerParameter *wm_chamber_max_temp;

// ── TLS + MQTT ─────────────────────────────────────────
WiFiClientSecure tlsClient;
PubSubClient     mqtt(tlsClient);
bool startupPortal = false;

// ── Data watchdog ──────────────────────────────────────
unsigned long lastDataMs = 0;

void dataReceived() {
  lastDataMs = millis();
}

void checkDataWatchdog() {
  if (millis() - lastDataMs > DATA_TIMEOUT_MS) {
    Serial.println("[Watchdog] No data received before timeout – Starting AP");
    // Turn off heater.
    resetHeaterControl();

    // Start the portal
    startPortal();

    // Failsafe reboot.
    delay(100);
    ESP.restart();
  }
}

// ──────────────────────────────────────────────────────
// Control helpers
// ──────────────────────────────────────────────────────
void heaterOn() {
  digitalWrite(RELAY_PIN, RELAY_ON);
  Serial.println("[Heater] ON");
}

void heaterOff() {
  digitalWrite(RELAY_PIN, RELAY_OFF);
  Serial.println("[Heater] OFF");
}

void fanOn() {
  analogWrite(FAN_PIN, FAN_FULL * (cfg.fanSpeedMax / 100.0f ));
  Serial.println("[Fan] ON");
}
void fanIdle() {
  analogWrite(FAN_PIN, FAN_FULL * (cfg.fanSpeedMin / 100.0f));
  Serial.println("[Fan] IDLE");
}

// ──────────────────────────────────────────────────────
// Main Control Logic
// ──────────────────────────────────────────────────────
bool chamberTripped = false; // Too hot, true if tripped
void resetHeaterControl(){
  //Reset the watchdog 
  dataReceived();
  //Reset the chamber latch
  chamberTripped = false;
  //Turn outputs off
  Serial.println("Controller Reset.") ;
  heaterOff();
  fanIdle();
}

void handleTemps(float bedSetpoint, float chamberTemp) {
  //Reset the watchdog
  dataReceived();

  //Print stats for debugging
  Serial.print("[Temps] Bed Setpoint: ");
  Serial.print(bedSetpoint, 1);
  Serial.print(" °C   Chamber: ");
  Serial.print(chamberTemp, 1);
  Serial.println(" °C");

  //Handle setting the chamber overtemperature latch
  if(!chamberTripped && chamberTemp >= cfg.chamberMaxTemp) {
    chamberTripped = true;
    Serial.println("[Logic] Chamber too hot, latch set.");
  }

  //Handle clearing the chamber overtemperature latch
  if (chamberTripped && chamberTemp <= (cfg.chamberMaxTemp - CHAMBER_RESET_TEMP_OFFSET)) {
    chamberTripped = false;
    Serial.println("[Logic] Chamber cooled, latch cleared.");
  }

  // Handle turning the heater on.
  if (bedSetpoint >= cfg.heaterOnTemp && bedSetpoint < MAX_BED_TEMP && !chamberTripped) {
    heaterOn();
  }else{
    heaterOff();
  }

  // Handle turning the fan on.
  if (chamberTemp >= cfg.fanOnTemp) {
    fanOn();
  }else{
    fanIdle();
  }
}

// ──────────────────────────────────────────────────────
// EEPROM helpers
// ──────────────────────────────────────────────────────
void eepromSave(const Config &c) {
  EEPROM.begin(sizeof(Config) + 1); // +1 for magic byte
  EEPROM.write(0, MAGIC_BYTE);
  EEPROM.put(1, c);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("[EEPROM] Config saved.");
}

bool eepromLoad(Config &c) {
  EEPROM.begin(sizeof(Config) + 1);
  bool ok = (EEPROM.read(0) == MAGIC_BYTE);
  if (ok) {
    EEPROM.get(1, c);
  }
  EEPROM.end();
  return ok;
}
// ──────────────────────────────────────────────────────
// Function to load config and set parameter defaults
// ──────────────────────────────────────────────────────
void setupWiFiManagerParams() {
    // Create parameters with values from loaded config
    wm_ip = new WiFiManagerParameter(
        "bbl_ip",           // HTML input name
        "Bambu printer IP", // Display label
        cfg.ip,             // Default value (from EEPROM)
        15                  // Max length
    );
    
    wm_code = new WiFiManagerParameter(
        "bbl_code",
        "Access code (8 char)",
        cfg.code,           // Default from EEPROM
        8
    );
    
    // Convert int values to strings for display
    static char heaterTempStr[4];
    snprintf(heaterTempStr, sizeof(heaterTempStr), "%d", cfg.heaterOnTemp);
    
    wm_bed_heater_temp = new WiFiManagerParameter(
        "bed_on",
        "Heater On Bed Temp (°C)",
        heaterTempStr,
        4
    );
    
    static char fanTempStr[4];
    snprintf(fanTempStr, sizeof(fanTempStr), "%d", cfg.fanOnTemp);
    
    wm_chamber_fan_temp = new WiFiManagerParameter(
        "bed_off",
        "Fan On Chamber Temp (°C)",
        fanTempStr,
        4
    );
    
    static char pwmMaxStr[4];
    snprintf(pwmMaxStr, sizeof(pwmMaxStr), "%d", cfg.fanSpeedMax);
    
    wm_pwm_max = new WiFiManagerParameter(
        "fan_max",
        "Max Fan Speed (%)",
        pwmMaxStr,
        4
    );
    
    static char pwmMinStr[4];
    snprintf(pwmMinStr, sizeof(pwmMinStr), "%d", cfg.fanSpeedMin);
    
    wm_pwm_min = new WiFiManagerParameter(
        "fan_min",
        "Min Fan Speed (%)",
        pwmMinStr,
        4
    );
    
    static char chamberTempStr[4];
    snprintf(chamberTempStr, sizeof(chamberTempStr), "%d", cfg.chamberMaxTemp);
    
    wm_chamber_max_temp = new WiFiManagerParameter(
        "cbr_max",
        "Chamber Max Temp (°C)",
        chamberTempStr,
        4
    );
}

// ──────────────────────────────────────────────────────
// Config portal
// ──────────────────────────────────────────────────────
void startPortal(void) {
  Serial.println("[Portal] Starting config AP: " PORTAL_SSID);

  //Teardown
  mqtt.disconnect();
  mqtt.setBufferSize(64);
  tlsClient.stop();
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);  // false = don't erase saved credentials
  WiFi.mode(WIFI_AP_STA);  // Explicitly set AP+STA mode
  delay(500);

  // First, setup parameters with current config values
  setupWiFiManagerParams();

  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_MANAGER_TIMEOUT);
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT);
  // Add our custom parameters
  wm.addParameter(wm_ip);
  wm.addParameter(wm_code);
  wm.addParameter(wm_bed_heater_temp);
  wm.addParameter(wm_chamber_fan_temp);
  wm.addParameter(wm_pwm_max);
  wm.addParameter(wm_pwm_min);
  wm.addParameter(wm_chamber_max_temp);

  // Set callback when user hits save
  wm.setSaveParamsCallback([] () {
    strncpy(cfg.ip,     wm_ip->getValue(),     15);  cfg.ip[15]   = '\0';
    strncpy(cfg.code,   wm_code->getValue(),    8);  cfg.code[8]  = '\0';

    //Check lower bounds (0 would mean on all the time.)
    cfg.heaterOnTemp = atoi(wm_bed_heater_temp->getValue());
    if(cfg.heaterOnTemp <= 0.00){
      cfg.heaterOnTemp = DEFAULT_HEATER_ON_BEDTEMP;
    }
    //Check lower bounds (0 would mean on all the time.)
    cfg.fanOnTemp = atoi(wm_chamber_fan_temp->getValue());
    if(cfg.fanOnTemp <= 0.00){
      cfg.fanOnTemp = DEFAULT_FAN_ON_CHAMBER_TEMP;
    }
    //Check upper bounds (Cannot exceed 70)
    cfg.chamberMaxTemp = atoi(wm_chamber_max_temp->getValue());
    if(cfg.chamberMaxTemp > DEFAULT_CHAMBER_TRIP_TEMP){
      cfg.chamberMaxTemp = DEFAULT_CHAMBER_TRIP_TEMP;
    }
    //Check upper bounds, invalid means we set to default.
    cfg.fanSpeedMax = atoi(wm_pwm_max->getValue());
    if(cfg.fanSpeedMax < DEFAULT_FAN_MIN_PWM ||  cfg.fanSpeedMax > DEFAULT_FAN_MAX_PWM){
       cfg.fanSpeedMax = DEFAULT_FAN_MAX_PWM;
    }
    //Check lower bounds - invalid means we set to default
    cfg.fanSpeedMin = atoi(wm_pwm_min->getValue());
    if(cfg.fanSpeedMin < DEFAULT_FAN_MIN_PWM ||  cfg.fanSpeedMin > DEFAULT_FAN_MAX_PWM){
       cfg.fanSpeedMin = DEFAULT_FAN_MIN_PWM;
    }else if(cfg.fanSpeedMin > cfg.fanSpeedMax){
      cfg.fanSpeedMin = cfg.fanSpeedMax;
    }
    printCfg();
    eepromSave(cfg);
  });

  // Blocking: shows portal until user configures & connects
  if (!wm.startConfigPortal(PORTAL_SSID, PORTAL_PASS)) {
    Serial.println("[Portal] Timed out, rebooting…");
  }else{
    Serial.println("[Portal] Done. Rebooting to apply config.");
  }
  //Reboot.
  delay(500);
  ESP.restart();
}

// ──────────────────────────────────────────────────────
// MQTT callback
// ──────────────────────────────────────────────────────
#define PROCESS_INTERVAL_MS 10000  // Only process a message every 5 seconds
unsigned long lastProcessMs = 0;
void onMessage(char* topic, byte* payload, unsigned int len) {
  //Only process periodically 
  unsigned long now = millis();
  if (now - lastProcessMs < PROCESS_INTERVAL_MS) return;

  //Check if topic is correct
  if (strstr(topic, "report") == NULL) {
    return;
  }

  // Reset counter
  lastProcessMs = now;

  //Use a filter to save memory
  StaticJsonDocument<256> filter;
  filter["print"]["device"]["ctc"]["info"]["temp"] = true;
  filter["print"]["bed_target_temper"] = true;

  StaticJsonDocument<256> doc;

  DeserializationError err = deserializeJson(doc, payload, len,
                                             DeserializationOption::Filter(filter));  
  if (err) {
    Serial.print("[JSON] Parse error: ");
    Serial.print(err.c_str());
    Serial.print("Msg Size: ");
    Serial.println(len);
    return;
  }

  float chamberTemp = doc["print"]["device"]["ctc"]["info"]["temp"] | -1.0f;
  float bedSetTemp  = doc["print"]["bed_target_temper"] | -1.0f;

  if (chamberTemp >= 0 && bedSetTemp >= 0) {
    handleTemps(bedSetTemp, chamberTemp);
    if(bedSetTemp == BED_TEMP_MAGIC_PORTAL){
      startupPortal = true;
    }
  }
}

// ──────────────────────────────────────────────────────
// MQTT connect / reconnect
// ──────────────────────────────────────────────────────
bool mqttConnect(void) {
  Serial.print("[MQTT] Connecting to ");
  Serial.print(cfg.ip);
  Serial.print(":");
  Serial.println(MQTT_PORT);
  startupPortal = false;

  // Bambu uses a self-signed cert, skip verification.
  tlsClient.setInsecure();
  tlsClient.setTimeout(30000); 
  // Config server
  mqtt.setServer(cfg.ip, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(MQTT_MAX_SIZE);
  
  // Client ID format expected by Bambu: any unique string
  String clientId = "SmartHeater_" + String(ESP.getChipId(), HEX);;
  //Connect and subscribe to all topics
  if (mqtt.connect(clientId.c_str(), MQTT_USER, cfg.code)) {
    mqtt.subscribe("#");
    return true;
  }
  //Error
  Serial.print("[MQTT] Failed, state=");
  Serial.println(mqtt.state());
  return false;
}

void printCfg() {
  Serial.println("[Config] Current configuration:");
  Serial.print("  IP: ");           Serial.println(cfg.ip);
  Serial.print("  Code: ");         Serial.println(cfg.code);
  Serial.print("  Heater On Temp: ");Serial.println(cfg.heaterOnTemp);
  Serial.print("  Fan On Temp: ");  Serial.println(cfg.fanOnTemp);
  Serial.print("  Chamber Max: ");  Serial.println(cfg.chamberMaxTemp);
  Serial.print("  Fan Max: ");      Serial.println(cfg.fanSpeedMax);
  Serial.print("  Fan Min: ");      Serial.println(cfg.fanSpeedMin);
}

void validateCfg() {
    //Check lower bounds (0 would mean on all the time.)
    if(cfg.heaterOnTemp <= 0.00){
      cfg.heaterOnTemp = DEFAULT_HEATER_ON_BEDTEMP;
    }
    //Check lower bounds (0 would mean on all the time.)
    if(cfg.fanOnTemp <= 0.00){
      cfg.fanOnTemp = DEFAULT_FAN_ON_CHAMBER_TEMP;
    }
    //Check upper bounds (Cannot exceed 70)
    if(cfg.chamberMaxTemp > DEFAULT_CHAMBER_TRIP_TEMP){
      cfg.chamberMaxTemp = DEFAULT_CHAMBER_TRIP_TEMP;
    }
    //Check upper bounds, invalid means we set to default.
    if(cfg.fanSpeedMax < DEFAULT_FAN_MIN_PWM ||  cfg.fanSpeedMax > DEFAULT_FAN_MAX_PWM){
       cfg.fanSpeedMax = DEFAULT_FAN_MAX_PWM;
    }
    //Check lower bounds - invalid means we set to default
    if(cfg.fanSpeedMin < DEFAULT_FAN_MIN_PWM ||  cfg.fanSpeedMin > DEFAULT_FAN_MAX_PWM){
       cfg.fanSpeedMin = DEFAULT_FAN_MIN_PWM;
    }else if(cfg.fanSpeedMin > cfg.fanSpeedMax){
      cfg.fanSpeedMin = cfg.fanSpeedMax;
    }
}

// ──────────────────────────────────────────────────────
// Setup
// ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[Boot] Bambu Smart Heater Control - V1.00");

  // Hardware setup
  pinMode(RELAY_PIN, OUTPUT);

  analogWriteFreq(FAN_FREQ);
  analogWriteRange(PWM_RANGE);
  pinMode(FAN_PIN, OUTPUT);

  // Hardware inital state
  analogWrite(FAN_PIN, FAN_OFF_DUTY);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  // 1. Try to load saved config from EEPROM
  bool hasCfg = eepromLoad(cfg);

  // 2. Validate Config
  validateCfg();
  printCfg();

  // 3. If no config at all, open portal
  if (!hasCfg ||
      strlen(cfg.ip) == 0 ||
      strlen(cfg.code) == 0) {
    Serial.println("[Boot] No config found - starting portal.");
    startPortal(); // does not return; reboots after save
  }
  
  // 4. Connect WiFi (STA mode, previously saved by WiFiManager)
  WiFi.mode(WIFI_STA);
  WiFi.begin();   // reconnects to last-known network
  Serial.print("[WiFi] Connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 20000) {
      Serial.println("\n[WiFi] Timeout → reopening portal.");
      startPortal();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.print("\n[WiFi] IP: ");
  Serial.println(WiFi.localIP());

  //Setup auto reconnnect
  WiFi.setAutoReconnect(true);

  // 5. First MQTT connection attempt
  if (!mqttConnect()) {
    Serial.println("[Boot] Initial MQTT connect failed.");
  }

  // 6. Make sure the data watchdog and all flags are reset.
  resetHeaterControl();
}

// ──────────────────────────────────────────────────────
// Loop
// ──────────────────────────────────────────────────────
unsigned long lastReconnect = 0;

void loop() {
  // Feed the watchdog
  checkDataWatchdog();
  
  //MQTT connected 
  if (mqtt.connected()) {
    mqtt.loop();   // handles keep-alive & incoming messages
    //Check for portal startup condition
    if (startupPortal) {
      startupPortal = false;
      startPortal();
    }
  }else{
    //Handle reconnect.
    unsigned long now = millis();
    if (now - lastReconnect > RECONNECT_MS) {
      lastReconnect = now;
      if (!mqttConnect()) {
        Serial.println("[MQTT] Reconnect failed.");
        return;
      }
    }
    //Sleep to not spam.
    delay(1000);
  }
}