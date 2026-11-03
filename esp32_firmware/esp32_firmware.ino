#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <time.h>

#include <WebServer.h> 
#include <ElegantOTA.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- 1. YOUR WIFI CREDENTIALS ---
#define WIFI_SSID "wifi_ssid"
#define WIFI_PASSWORD "wifi_password"

// --- 2. YOUR FIREBASE CREDENTIALS ---
#define API_KEY "api_key"
#define DATABASE_URL "project-id.firebaseio.com"

// --- 3. THE ESP32'S LOGIN ACCOUNT ---
#define USER_EMAIL "email"
#define USER_PASSWORD "password"

// --- HARDWARE PINS ---
#define TIMER_RELAY_PIN 21       // 120V Intermatic Timer Bypass
#define VALVE_RELAY_PIN 22       // 24V Valve Actuators
#define START_BUTTON_RELAY 19    // 120V Spa Start Button (PULSE)
#define WARNING_LED_PIN 25       // 15-Min Warning Light

WebServer server(80); 

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Timers and State variables
unsigned long sendDataPrevMillis = 0;
unsigned long shutdownUpdateMillis = 0;
unsigned long lastWiFiCheckMillis = 0;

bool isSystemOn = false;
bool isHeaterOnly = false;
double targetFurnaceOffTime = 0;
bool isScheduled = false;
double scheduledStartTime = 0;
double scheduledDurationMs = 0;
double triggerDurationMs = 0; 
bool triggerStop = false; 
bool startPulseSent = false; 

void setup() {
  Serial.begin(115200);
  
  pinMode(TIMER_RELAY_PIN, OUTPUT);
  pinMode(VALVE_RELAY_PIN, OUTPUT);
  pinMode(START_BUTTON_RELAY, OUTPUT);
  pinMode(WARNING_LED_PIN, OUTPUT);
  
  digitalWrite(TIMER_RELAY_PIN, LOW);
  digitalWrite(VALVE_RELAY_PIN, LOW); 
  digitalWrite(START_BUTTON_RELAY, LOW); 
  digitalWrite(WARNING_LED_PIN, LOW); 

  Serial.print("Connecting to Wi-Fi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  // Power Outage Boot Watchdog
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 100) { 
    Serial.print("."); 
    delay(500); 
    wifiAttempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi timed out (Router likely booting). Rebooting ESP32...");
    ESP.restart(); 
  }
  Serial.println("\nConnected!");
  Serial.print("IP Address for OTA Updates: ");
  Serial.println(WiFi.localIP()); 

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", "Resort Remote ESP32 is running! Go to /update to flash new code.");
  });
  
  ElegantOTA.begin(&server); 
  server.begin();

  configTime(0, 0, "pool.ntp.org");
  time_t now = time(nullptr);
  while (now < 100000) { delay(500); now = time(nullptr); }

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback; 
  config.timeout.socketConnection = 10 * 1000; 

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  server.handleClient();
  ElegantOTA.loop();

  // Watchdog to fix dropouts
  if (millis() - lastWiFiCheckMillis > 10000) {
    lastWiFiCheckMillis = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi dropped. Restarting ESP32 to clear buffer...");
      ESP.restart(); 
    }
  }

  // FETCH DATA
  if (Firebase.ready() && (millis() - sendDataPrevMillis > 2000 || sendDataPrevMillis == 0)) {
    sendDataPrevMillis = millis();

    if (Firebase.RTDB.getJSON(&fbdo, "system")) {
      FirebaseJson &json = fbdo.jsonObject();
      FirebaseJsonData jsonData;

      json.get(jsonData, "isSystemOn");
      if (jsonData.success) isSystemOn = jsonData.to<bool>();
      
      json.get(jsonData, "isHeaterOnly");
      if (jsonData.success) isHeaterOnly = jsonData.to<bool>();

      json.get(jsonData, "targetShutdownTime");
      if (jsonData.success) targetFurnaceOffTime = jsonData.to<double>();

      json.get(jsonData, "isScheduled");
      if (jsonData.success) isScheduled = jsonData.to<bool>();

      json.get(jsonData, "scheduledStartTime");
      if (jsonData.success) scheduledStartTime = jsonData.to<double>();

      json.get(jsonData, "scheduledDurationMs");
      if (jsonData.success) scheduledDurationMs = jsonData.to<double>();

      json.get(jsonData, "triggerDurationMs");
      if (jsonData.success) triggerDurationMs = jsonData.to<double>();

      json.get(jsonData, "triggerStop");
      if (jsonData.success) triggerStop = jsonData.to<bool>();
    }
  }

  time_t now = time(nullptr);
  double currentMs = (double)now * 1000.0;

  if (triggerDurationMs > 0) {
    double newShutdownTime = currentMs + triggerDurationMs;
    Firebase.RTDB.setBool(&fbdo, "system/isSystemOn", true);
    Firebase.RTDB.setDouble(&fbdo, "system/targetShutdownTime", newShutdownTime);
    Firebase.RTDB.setDouble(&fbdo, "system/triggerDurationMs", 0); 
    
    isSystemOn = true;
    targetFurnaceOffTime = newShutdownTime;
    triggerDurationMs = 0; 
  }

  if (triggerStop) {
    targetFurnaceOffTime = currentMs - 1000; 
    Firebase.RTDB.setDouble(&fbdo, "system/targetShutdownTime", targetFurnaceOffTime);
    Firebase.RTDB.setBool(&fbdo, "system/triggerStop", false);
    triggerStop = false; 
  }

  if (isScheduled && !isSystemOn) {
    if (currentMs >= scheduledStartTime) {
      double newShutdownTime = currentMs + scheduledDurationMs;
      Firebase.RTDB.setBool(&fbdo, "system/isSystemOn", true);
      Firebase.RTDB.setDouble(&fbdo, "system/targetShutdownTime", newShutdownTime);
      Firebase.RTDB.setBool(&fbdo, "system/isScheduled", false);
      Firebase.RTDB.setBool(&fbdo, "system/isHeaterOnly", false);
      
      isSystemOn = true;
      isHeaterOnly = false;
      targetFurnaceOffTime = newShutdownTime;
      isScheduled = false;
    }
  }

  if (isSystemOn) {
    double targetValvesOffTime = targetFurnaceOffTime + 1800000.0; // 30 min cooldown

    if (currentMs < targetFurnaceOffTime) {
      // 1. ACTIVE PHASE
      digitalWrite(TIMER_RELAY_PIN, HIGH);     // Furnace/Main Pump ON
      
      if (isHeaterOnly) {
        digitalWrite(VALVE_RELAY_PIN, LOW);    // Valves Closed
      } else {
        digitalWrite(VALVE_RELAY_PIN, HIGH);   // Valves Open
      }
      
      // Pulse to turn ON the Spa system (Required for both normal and heater modes)
      if (!startPulseSent) {
        delay(500); 
        digitalWrite(START_BUTTON_RELAY, HIGH); 
        delay(1000); 
        digitalWrite(START_BUTTON_RELAY, LOW);  
        startPulseSent = true; 
      }

      double remainingMs = targetFurnaceOffTime - currentMs;
      if (remainingMs <= 60000) {
        if ((millis() / 500) % 2 == 0) digitalWrite(WARNING_LED_PIN, HIGH);
        else digitalWrite(WARNING_LED_PIN, LOW); 
      } else if (remainingMs <= 900000) {
        digitalWrite(WARNING_LED_PIN, HIGH); 
      } else {
        digitalWrite(WARNING_LED_PIN, LOW);
      }
      
    } else if (currentMs >= targetFurnaceOffTime && currentMs < targetValvesOffTime) {
      // 2. COOLDOWN PHASE
      digitalWrite(TIMER_RELAY_PIN, LOW);      // Drops master power, killing jets and furnace!
      digitalWrite(WARNING_LED_PIN, LOW); 
      
      digitalWrite(START_BUTTON_RELAY, LOW);   // Ensure pulse relay is off
      startPulseSent = false;                  // Reset state since power was dropped

      if (isHeaterOnly) {
        digitalWrite(VALVE_RELAY_PIN, LOW);    // Valves Stay Closed
      } else {
        digitalWrite(VALVE_RELAY_PIN, HIGH);   // Valves Stay Open for cooldown
      }
      
    } else {
      // 3. CYCLE FINISHED - WAIT FOR RESET
      digitalWrite(TIMER_RELAY_PIN, LOW);
      digitalWrite(VALVE_RELAY_PIN, LOW);
      digitalWrite(START_BUTTON_RELAY, LOW);
      digitalWrite(WARNING_LED_PIN, LOW);
      startPulseSent = false; 
      
      if (millis() - shutdownUpdateMillis > 5000) {
        Firebase.RTDB.setBool(&fbdo, "system/isSystemOn", false);
        Firebase.RTDB.setInt(&fbdo, "system/targetShutdownTime", 0);
        Firebase.RTDB.setBool(&fbdo, "system/isHeaterOnly", false); 
        shutdownUpdateMillis = millis();
      }
    }
  } else {
    // SYSTEM IDLE OR EMERGENCY STOPPED
    // Dropping TIMER_RELAY_PIN to LOW breaks the physical holding circuit,
    // which automatically kills the main pump, furnace, and the jets!
    digitalWrite(TIMER_RELAY_PIN, LOW);
    digitalWrite(VALVE_RELAY_PIN, LOW);
    digitalWrite(WARNING_LED_PIN, LOW);
    digitalWrite(START_BUTTON_RELAY, LOW);
    startPulseSent = false; 
  }
}
