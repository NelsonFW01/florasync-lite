/*
 * FloraSync Lite — ESP32 Firmware
 * Binus University 2026
 * 
 * Library yang diperlukan (install via Arduino Library Manager):
 * - Firebase ESP Client by Mobizt
 * - DHT sensor library by Adafruit
 * - Adafruit Unified Sensor by Adafruit
 */

#include <DHT.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ========== PIN DEFINITIONS ==========
#define DHTPIN        4
#define DHTTYPE       DHT22
#define SOIL_PIN      35
#define LDR_PIN       34
#define TRIG_PIN      18
#define ECHO_PIN      19
#define RELAY_PUMP    23
#define RELAY_FAN     22

// ========== GANTI DENGAN WIFI MILIK KALIAN ==========
#define WIFI_SSID         "NAMA_WIFI_KALIAN"
#define WIFI_PASSWORD     "PASSWORD_WIFI_KALIAN"
#define API_KEY           "FIREBASE_API_KEY_KALIAN"
#define DATABASE_URL      "FIREBASE_DATABASE_URL_KALIAN"
// ================================================

DHT dht(DHTPIN, DHTTYPE);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ========== PUMP CONTROL ==========
const int     SOIL_THRESHOLD   = 2200;   // ADC; semakin kecil = makin kering
const unsigned long PUMP_DURATION  = 10000;  // ms (10 detik)
const unsigned long PUMP_COOLDOWN  = 60000;  // ms (1 menit)
const unsigned long FAN_AFTER_PUMP = 90000;  // ms (1.5 menit pasca siram)

bool pumpActive    = false;
bool hasEverPumped = false;
unsigned long pumpStart = 0;
unsigned long pumpStop  = 0;

// ========== TIMING ==========
const unsigned long SENSOR_INTERVAL  = 5000;   // baca sensor tiap 5 detik
const unsigned long FIREBASE_INTERVAL = 5000;  // kirim ke Firebase tiap 5 detik
const unsigned long HISTORY_INTERVAL  = 60000; // simpan history tiap 1 menit

unsigned long lastSensor  = 0;
unsigned long lastFirebase = 0;
unsigned long lastHistory  = 0;

bool signupOK = false;

// ========== SENSOR VALUES ==========
float temperature = 0;
float humidity    = 0;
int   soilValue   = 0;
int   ldrValue    = 0;
float distance    = 0;

void setup() {
  Serial.begin(115200);
  dht.begin();

  pinMode(SOIL_PIN, INPUT);
  pinMode(LDR_PIN,  INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PUMP, OUTPUT);
  pinMode(RELAY_FAN,  OUTPUT);

  // Relay module active LOW: HIGH = OFF saat init
  digitalWrite(RELAY_PUMP, HIGH);
  digitalWrite(RELAY_FAN,  HIGH);

  // ========== WIFI CONNECT ==========
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan WiFi");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    Serial.print(".");
    delay(500);
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi terhubung: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nGagal konek WiFi! Cek SSID/Password.");
  }

  // ========== FIREBASE INIT ==========
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase signup OK (anonymous)");
    signupOK = true;
  } else {
    Serial.printf("Firebase signup error: %s\n",
      config.signer.signupError.message.c_str());
  }

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("FloraSync Lite siap!");
  Serial.println("===================");
}

// ========== READ ALL SENSORS ==========
void readSensors() {
  // DHT22
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    humidity    = h;
    temperature = t;
  } else {
    Serial.println("[DHT22] Error baca sensor!");
  }

  // Soil moisture (capacitive)
  soilValue = analogRead(SOIL_PIN);

  // LDR
  ldrValue = analogRead(LDR_PIN);

  // Ultrasonic HC-SR04
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long dur = pulseIn(ECHO_PIN, HIGH, 30000); // timeout 30ms agar tidak hang
  if (dur == 0) {
    Serial.println("[HC-SR04] Tidak ada echo, cek kabel.");
    distance = -1;
  } else {
    distance = dur * 0.034 / 2.0;
  }

  // Print ke Serial Monitor
  Serial.println("--- Sensor Reading ---");
  Serial.printf("Suhu: %.1f °C | Kelembaban: %.1f %%\n", temperature, humidity);
  Serial.printf("Soil ADC: %d | LDR ADC: %d\n", soilValue, ldrValue);
  Serial.printf("Level Air: %.1f cm\n", distance);
}

// ========== ACTUATOR CONTROL ==========
void controlActuators() {
  unsigned long now = millis();

  // --- Cek timer pompa ---
  if (pumpActive && (now - pumpStart >= PUMP_DURATION)) {
    digitalWrite(RELAY_PUMP, HIGH); // OFF
    pumpActive     = false;
    hasEverPumped  = true;
    pumpStop       = now;
    Serial.println("[PUMP] OFF — timer 10 detik selesai");
  }

  // --- Logika pompa ---
  bool cooldownActive = hasEverPumped && !pumpActive &&
                        (now - pumpStop < PUMP_COOLDOWN);

  if (soilValue < SOIL_THRESHOLD) {
    if (pumpActive) {
      Serial.println("[PUMP] Sedang menyiram...");
    } else if (cooldownActive) {
      unsigned long sisa = (PUMP_COOLDOWN - (now - pumpStop)) / 1000;
      Serial.printf("[PUMP] Cooldown aktif, tunggu %lu detik\n", sisa);
    } else {
      digitalWrite(RELAY_PUMP, LOW); // ON
      pumpActive = true;
      pumpStart  = now;
      Serial.println("[PUMP] ON — tanah kering, mulai menyiram 10 detik");
    }
  } else {
    if (pumpActive) {
      digitalWrite(RELAY_PUMP, HIGH); // OFF lebih awal
      pumpActive    = false;
      hasEverPumped = true;
      pumpStop      = now;
      Serial.println("[PUMP] OFF — tanah sudah basah");
    } else {
      Serial.println("[PUMP] OFF — tanah cukup basah");
    }
  }

  // --- Logika kipas ---
  bool pascaSiram = hasEverPumped && !pumpActive &&
                    (now - pumpStop < FAN_AFTER_PUMP);

  bool fanShouldOn = (!isnan(temperature) && temperature > 30.0) ||
                     (!isnan(humidity)    && humidity    > 80.0) ||
                     pascaSiram;

  if (fanShouldOn) {
    digitalWrite(RELAY_FAN, LOW); // ON
    Serial.println("[FAN] ON");
  } else {
    digitalWrite(RELAY_FAN, HIGH); // OFF
    Serial.println("[FAN] OFF");
  }

  Serial.println();
}

// ========== SEND TO FIREBASE ==========
void sendToFirebase() {
  if (!signupOK || !Firebase.ready()) {
    Serial.println("[Firebase] Tidak siap, skip kirim.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Terputus! Mencoba reconnect...");
    WiFi.reconnect();
    return;
  }

  bool pumpStatus = (digitalRead(RELAY_PUMP) == LOW);
  bool fanStatus  = (digitalRead(RELAY_FAN)  == LOW);

  // Tulis ke /sensor (data real-time terbaru)
  Firebase.RTDB.setFloat(&fbdo,  "/sensor/temperature", temperature);
  Firebase.RTDB.setFloat(&fbdo,  "/sensor/humidity",    humidity);
  Firebase.RTDB.setInt(&fbdo,    "/sensor/soilMoisture", soilValue);
  Firebase.RTDB.setInt(&fbdo,    "/sensor/ldrValue",    ldrValue);
  Firebase.RTDB.setFloat(&fbdo,  "/sensor/waterLevel",  distance);
  Firebase.RTDB.setBool(&fbdo,   "/actuator/pump",      pumpStatus);
  Firebase.RTDB.setBool(&fbdo,   "/actuator/fan",       fanStatus);

  Serial.println("[Firebase] Data sensor terkirim.");
}

// ========== SAVE TO HISTORY ==========
// Menyimpan snapshot ke /history/{timestamp} — dipakai oleh halaman History & Export
void saveToHistory() {
  if (!signupOK || !Firebase.ready()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long ts = millis(); // gunakan millis sebagai key sementara
  // Sebaiknya pakai timestamp Unix dari NTP di production
  // Untuk prototipe, millis() cukup karena web akan sort berdasarkan key

  String path = "/history/" + String(ts);

  bool pumpStatus = (digitalRead(RELAY_PUMP) == LOW);
  bool fanStatus  = (digitalRead(RELAY_FAN)  == LOW);

  FirebaseJson json;
  json.set("temperature", temperature);
  json.set("humidity",    humidity);
  json.set("soilMoisture", soilValue);
  json.set("ldrValue",    ldrValue);
  json.set("waterLevel",  distance);
  json.set("pump",        pumpStatus);
  json.set("fan",         fanStatus);

  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
    Serial.println("[Firebase] History tersimpan: " + path);
  } else {
    Serial.println("[Firebase] Gagal simpan history: " + fbdo.errorReason());
  }
}

// ========== MAIN LOOP ==========
void loop() {
  unsigned long now = millis();

  // Baca sensor & kontrol aktuator tiap SENSOR_INTERVAL
  if (now - lastSensor >= SENSOR_INTERVAL) {
    lastSensor = now;
    readSensors();
    controlActuators();
  }

  // Kirim ke Firebase tiap FIREBASE_INTERVAL
  if (now - lastFirebase >= FIREBASE_INTERVAL) {
    lastFirebase = now;
    sendToFirebase();
  }

  // Simpan history tiap HISTORY_INTERVAL
  if (now - lastHistory >= HISTORY_INTERVAL) {
    lastHistory = now;
    saveToHistory();
  }
}
