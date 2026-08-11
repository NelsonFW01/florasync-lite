#include <DHT.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <esp_task_wdt.h>   // watchdog timer, restart otomatis kalau loop() macet
#include "esp_system.h"     // untuk esp_reset_reason()

// ========== PIN DEFINITIONS ==========
#define DHTPIN        4
#define DHTTYPE       DHT22
#define SOIL_PIN      35
#define LDR_PIN       34
#define TRIG_PIN      18
#define ECHO_PIN      19
#define RELAY_PUMP    22
#define RELAY_FAN     23

// ========== GANTI DENGAN MILIK KALIAN ==========
// Kredensial nyata JANGAN ditaruh di sini. Copy secrets.h.example -> secrets.h,
// isi di sana, dan pastikan secrets.h masuk .gitignore.
#include "secrets.h"   // berisi WIFI_SSID, WIFI_PASSWORD, DEVICE_EMAIL, DEVICE_PASSWORD

#define API_KEY      "AIzaSyAP5lGx8vTPw0pcTyxdx1kBtavbDpVmflE"
#define DATABASE_URL "https://florasync-lite-default-rtdb.asia-southeast1.firebasedatabase.app"
// ================================================

DHT dht(DHTPIN, DHTTYPE);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ========== PUMP CONTROL (dengan HYSTERESIS) ==========
const int SOIL_THRESHOLD_ON  = 2000;   // tanah kering → nyalakan pompa
const int SOIL_THRESHOLD_OFF = 2300;   // tanah basah → matikan pompa
const unsigned long PUMP_DURATION  = 10000;  // 10 detik
const unsigned long PUMP_COOLDOWN  = 60000;  // 1 menit
const unsigned long FAN_AFTER_PUMP = 90000;  // 1.5 menit

bool pumpActive    = false;
bool hasEverPumped = false;
unsigned long pumpStart = 0;
unsigned long pumpStop  = 0;

// ========== FAN CONTROL (dengan HYSTERESIS) ==========
const float TEMP_FAN_ON   = 31.0;   // kipas ON jika suhu > 31°C
const float TEMP_FAN_OFF  = 29.0;   // kipas OFF jika suhu < 29°C
const float HUM_FAN_ON    = 82.0;   // kipas ON jika RH > 82%
const float HUM_FAN_OFF   = 78.0;   // kipas OFF jika RH < 78%
bool fanState = false;

// ========== TIMING ==========
const unsigned long SENSOR_INTERVAL   = 5000;
const unsigned long FIREBASE_INTERVAL = 5000;
const unsigned long HISTORY_INTERVAL  = 60000;
const unsigned long RELAY_STAGGER_MS  = 150;

unsigned long lastSensor  = 0;
unsigned long lastFirebase = 0;
unsigned long lastHistory  = 0;

bool signupOK = false;

// ========== AKURASI SENSOR ==========
#define SOIL_SAMPLES        10   // jumlah sampel ADC soil per pembacaan
#define LDR_SAMPLES         10   // jumlah sampel ADC LDR per pembacaan
#define ULTRASONIC_SAMPLES  5    // jumlah tembakan HC-SR04 per pembacaan

// ========== SENSOR VALUES (dengan fallback) ==========
float temperature = 0;
float humidity    = 0;
int   soilValue   = 0;
int   ldrValue    = 0;
float distance    = -1;

// Nilai terakhir yang valid dari DHT22
float lastValidTemp = 0;
float lastValidHum  = 0;

// ========== PROTEKSI DRY-RUN POMPA / AIR HABIS ==========
// Sensor HC-SR04 dipasang di ATAS tandon menghadap ke bawah dan mengukur
// jarak ke PERMUKAAN AIR (bukan tinggi air dari dasar), jadi jarak makin
// JAUH = air makin SEDIKIT.
// WATER_LEVEL_EMPTY_CM = jarak sensor ke dasar tandon saat kosong = 18,5 cm.
// WATER_LEVEL_OK_CM = jarak sensor ke air saat tinggi air 6 cm dari dasar
// (batas aman yang ditentukan) -> 18,5 - 6 = 12,5 cm.
// Kalau batas amannya mau diganti nanti: WATER_LEVEL_OK_CM =
// WATER_LEVEL_EMPTY_CM dikurangi tinggi air minimum yang diinginkan (cm).
const float WATER_LEVEL_EMPTY_CM = 18.5;
const float WATER_LEVEL_OK_CM    = 12.5;
const int   MAX_ULTRASONIC_FAULT_TRUST = 3; 

bool  waterEmpty        = true;  // default aman saat boot: anggap habis sampai terbukti ada air
float lastValidDistance = -1;

// ========== ERROR COUNTER UNTUK RESTART ==========
int wifiFailCount = 0;
int firebaseFailCount = 0;
const int MAX_WIFI_FAIL = 4;
const int MAX_FB_FAIL   = 10;

// ========== COUNTER DIAGNOSTIK ==========
int dhtFaultCount        = 0;
int ultrasonicFaultCount = 0;
int relayMismatchCount   = 0;

unsigned long pumpCycleCount      = 0;
unsigned long fanCycleCount       = 0;
unsigned long pumpTotalRuntimeMs  = 0;
unsigned long fanTotalRuntimeMs   = 0;
unsigned long fanOnStart          = 0;
unsigned long pumpDryRunStopCount = 0; // berapa kali pompa dipaksa OFF karena air habis

// ---------- Fungsi restart aman ----------
void safeRestart() {
  Serial.println("[SYSTEM] Restart ESP32...");
  delay(1000);
  ESP.restart();
}

// ---------- Cetak alasan restart terakhir ----------
void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("[BOOT] Alasan restart terakhir: ");
  switch (reason) {
    case ESP_RST_POWERON:
      Serial.println("Power-on normal (colok listrik / reset manual)");
      break;
    case ESP_RST_EXT:
      Serial.println("Reset via pin eksternal");
      break;
    case ESP_RST_SW:
      Serial.println("Software reset (ESP.restart() dipanggil oleh kode)");
      break;
    case ESP_RST_PANIC:
      Serial.println("PANIC/crash pada kode (exception)");
      break;
    case ESP_RST_INT_WDT:
      Serial.println("Interrupt watchdog timeout");
      break;
    case ESP_RST_TASK_WDT:
      Serial.println("Task watchdog timeout (loop() sempat macet)");
      break;
    case ESP_RST_WDT:
      Serial.println("Watchdog lain timeout");
      break;
    case ESP_RST_BROWNOUT:
      Serial.println("!!! BROWNOUT — tegangan sempat drop di bawah batas aman !!!");
      Serial.println("     -> Kemungkinan besar power supply kurang kuat saat");
      Serial.println("        pompa & kipas (atau bebannya) narik arus bersamaan.");
      break;
    case ESP_RST_DEEPSLEEP:
      Serial.println("Bangun dari deep sleep");
      break;
    default:
      Serial.print("Lainnya/tidak diketahui (kode=");
      Serial.print((int)reason);
      Serial.println(")");
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  printResetReason();

  dht.begin();

  pinMode(SOIL_PIN, INPUT);
  pinMode(LDR_PIN,  INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PUMP, OUTPUT);
  pinMode(RELAY_FAN,  OUTPUT);

  // Kunci resolusi & attenuation ADC secara eksplisit supaya pembacaan
  // soil/LDR konsisten, tidak bergantung default core Arduino-ESP32.
  analogReadResolution(12);        // 0-4095
  analogSetAttenuation(ADC_11db);  // rentang input ~0-3.3V (sesuai devkit umum)

  digitalWrite(RELAY_PUMP, HIGH); // OFF
  digitalWrite(RELAY_FAN,  HIGH); // OFF

  // ========== WIFI CONNECT ==========
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan WiFi");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 40) {
    Serial.print(".");
    delay(500);
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi terhubung: " + WiFi.localIP().toString());
    wifiFailCount = 0;
  } else {
    Serial.println("\nGagal konek WiFi! Restart...");
    safeRestart();
  }

  // ========== FIREBASE INIT ==========
  // Pakai akun device TETAP (email/password), bukan anonymous, supaya UID
  // stabil dan bisa di-whitelist di rules ("/admins/<uid>": true).
  // Akun ini dibuat sekali secara manual di Firebase Console > Authentication.
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  auth.user.email    = DEVICE_EMAIL;
  auth.user.password = DEVICE_PASSWORD;
  signupOK = true; // sign-in ditangani otomatis oleh Firebase.begin() di bawah

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // ========== AKTIFKAN TASK WATCHDOG ==========
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 600000,    // 10 menit timeout
    .trigger_panic = true    // restart otomatis jika hang
  };
  esp_task_wdt_init(&twdt_config);
  esp_task_wdt_add(NULL);

  Serial.println("FloraSync Lite siap.");
  Serial.println("===================");
}

// ========== HELPER: SOIL MOISTURE (trimmed mean) ==========
int readSoilAveraged() {
  int samples[SOIL_SAMPLES];
  for (int i = 0; i < SOIL_SAMPLES; i++) {
    samples[i] = analogRead(SOIL_PIN);
    delayMicroseconds(500);
  }
  // insertion sort (aman & cukup cepat untuk N sekecil ini)
  for (int i = 1; i < SOIL_SAMPLES; i++) {
    int key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = key;
  }
  // buang nilai tertinggi & terendah, rata-ratakan sisanya
  long sum = 0;
  int count = 0;
  for (int i = 1; i < SOIL_SAMPLES - 1; i++) {
    sum += samples[i];
    count++;
  }
  return (int)(sum / count);
}

// ========== HELPER: LDR (trimmed mean) ==========
int readLdrAveraged() {
  int samples[LDR_SAMPLES];
  for (int i = 0; i < LDR_SAMPLES; i++) {
    samples[i] = analogRead(LDR_PIN);
    delayMicroseconds(500);
  }
  for (int i = 1; i < LDR_SAMPLES; i++) {
    int key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = key;
  }
  long sum = 0;
  int count = 0;
  for (int i = 1; i < LDR_SAMPLES - 1; i++) {
    sum += samples[i];
    count++;
  }
  return (int)(sum / count);
}

// ========== HELPER: HC-SR04 (multi-sample + median filter) ==========
float readUltrasonicFiltered() {
  float validSamples[ULTRASONIC_SAMPLES];
  int validCount = 0;

  // Cepat rambat suara berubah ~0,6 m/s tiap 1°C. Pakai suhu DHT22
  // terkini (bukan konstanta tetap) supaya konversi durasi pantulan ke
  // jarak lebih akurat — penting karena rentang tandonnya cuma ~18 cm.
  float soundSpeedCmPerUs = (331.3 + 0.606 * temperature) / 10000.0;

  for (int i = 0; i < ULTRASONIC_SAMPLES; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long dur = pulseIn(ECHO_PIN, HIGH, 30000);
    if (dur > 0) {
      float d = dur * soundSpeedCmPerUs / 2.0;
      // Rentang wajar HC-SR04 ~2-400 cm; di luar itu kemungkinan noise/echo salah.
      if (d >= 2.0 && d <= 400.0) {
        validSamples[validCount++] = d;
      }
    }
    delay(10); // cegah tembakan berikutnya bentrok dengan echo sebelumnya
  }

  if (validCount == 0) {
    ultrasonicFaultCount++;
    Serial.println("[HC-SR04] Semua sampel gagal/di luar rentang wajar");
    return -1;
  }

  for (int i = 1; i < validCount; i++) {
    float key = validSamples[i];
    int j = i - 1;
    while (j >= 0 && validSamples[j] > key) {
      validSamples[j + 1] = validSamples[j];
      j--;
    }
    validSamples[j + 1] = key;
  }

  ultrasonicFaultCount = 0;
  return validSamples[validCount / 2]; // median -> tahan terhadap outlier
}

// ========== HELPER: TULIS RELAY + VERIFIKASI ==========
// CATATAN: ini memverifikasi bahwa GPIO ESP32 benar-benar berubah level,
// BUKAN memverifikasi kondisi fisik pompa/kipas (tidak ada sensor arus/
// feedback fisik di rangkaian ini). Untuk itu perlu tambahan hardware
// (mis. sensor arus ACS712 atau kontak feedback dari modul relay).
bool writeRelayVerified(int pin, bool activate) {
  int targetLevel = activate ? LOW : HIGH;
  digitalWrite(pin, targetLevel);
  delay(5);
  if (digitalRead(pin) != targetLevel) {
    digitalWrite(pin, targetLevel); // retry sekali
    delay(5);
    if (digitalRead(pin) != targetLevel) {
      relayMismatchCount++;
      Serial.printf("[RELAY] WARNING: pin %d gagal berubah ke level %d\n", pin, targetLevel);
      return false;
    }
  }
  return true;
}

// ========== HELPER: STATUS AIR HABIS UNTUK PROTEKSI DRY-RUN ==========
// Update variabel global 'waterEmpty' dari pembacaan HC-SR04 terbaru (distance).
// - Hysteresis (mirip soil/suhu/RH): WATER_LEVEL_EMPTY_CM vs WATER_LEVEL_OK_CM.
// - Fallback ke lastValidDistance kalau pembacaan saat ini gagal (mirip DHT22).
// - Kalau gagal berturut-turut >= MAX_ULTRASONIC_FAULT_TRUST, data lama
//   dianggap tidak bisa dipercaya lagi -> fail-safe, anggap air HABIS supaya
//   pompa tidak jalan berdasarkan data yang sudah basi.
void updateWaterEmptyStatus() {
  if (distance > 0) {
    lastValidDistance = distance;
  }

  if (ultrasonicFaultCount >= MAX_ULTRASONIC_FAULT_TRUST) {
    waterEmpty = true;
    Serial.println("[WATER] Sensor gagal berturut-turut, data basi -> asumsi HABIS (fail-safe)");
    return;
  }

  if (lastValidDistance <= 0) {
    // Belum pernah ada pembacaan valid sejak boot.
    waterEmpty = true;
    return;
  }

  if (lastValidDistance >= WATER_LEVEL_EMPTY_CM) {
    waterEmpty = true;
  } else if (lastValidDistance <= WATER_LEVEL_OK_CM) {
    waterEmpty = false;
  }
  // else: zona hysteresis tengah -> status sebelumnya dipertahankan (sengaja)
}

// ========== BACA SEMUA SENSOR (dengan retry & filter) ==========
void readSensors() {
  // DHT22 — coba 3 kali, rata-ratakan SEMUA pembacaan yang valid (bukan cuma
  // pakai percobaan pertama yang sukses) supaya lebih tahan jitter sensor.
  float hSum = 0, tSum = 0;
  int validReads = 0;
  for (int i = 0; i < 3; i++) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h) && !isnan(t)) {
      hSum += h;
      tSum += t;
      validReads++;
    }
    delay(100);
  }

  if (validReads > 0) {
    temperature   = tSum / validReads;
    humidity      = hSum / validReads;
    lastValidTemp = temperature;
    lastValidHum  = humidity;
    dhtFaultCount = 0;
    Serial.printf("[DHT22] OK (%d/3 valid, dirata-rata)\n", validReads);
  } else {
    temperature = lastValidTemp;
    humidity    = lastValidHum;
    dhtFaultCount++;
    Serial.println("[DHT22] Error, pakai data terakhir");
  }

  soilValue = readSoilAveraged();
  ldrValue  = readLdrAveraged();
  distance  = readUltrasonicFiltered();

  updateWaterEmptyStatus();

  Serial.println("--- Sensor Reading ---");
  Serial.printf("Suhu: %.1f °C | Kelembaban: %.1f %%\n", temperature, humidity);
  Serial.printf("Soil ADC (avg): %d | LDR ADC (avg): %d\n", soilValue, ldrValue);
  Serial.printf("Level Air (median): %.1f cm | Status: %s\n", distance, waterEmpty ? "HABIS/KRITIS" : "Cukup");

  if (WiFi.status() != WL_CONNECTED) {
    wifiFailCount++;
    Serial.printf("[WiFi] Gagal (%d/%d)\n", wifiFailCount, MAX_WIFI_FAIL);
    if (wifiFailCount >= MAX_WIFI_FAIL) {
      safeRestart();
    }
  } else {
    wifiFailCount = 0;
  }
}

// ========== ACTUATOR CONTROL (HYSTERESIS + STAGGER + VERIFIKASI + COUNTER) ==========
void controlActuators() {
  unsigned long now = millis();

  bool pumpWasActive = pumpActive;
  bool fanWasActive  = fanState;

  // --- Proteksi dry-run: air habis SAAT pompa nyala -> paksa OFF sekarang
  // juga, jangan tunggu PUMP_DURATION selesai. ---
  if (pumpActive && waterEmpty) {
    pumpActive     = false;
    hasEverPumped  = true;
    pumpStop       = now;
    pumpDryRunStopCount++;
    Serial.println("[PUMP] OFF — AIR HABIS! Proteksi dry-run, paksa berhenti sebelum timer selesai.");
  }
  // --- Status pompa (logika hysteresis) ---
  else if (pumpActive && (now - pumpStart >= PUMP_DURATION)) {
    pumpActive     = false;
    hasEverPumped  = true;
    pumpStop       = now;
    Serial.println("[PUMP] OFF — timer 10 detik selesai");
  }

  bool cooldownActive = hasEverPumped && !pumpActive &&
                        (now - pumpStop < PUMP_COOLDOWN);

  // Syarat tambahan !waterEmpty: pompa tidak boleh menyala kalau air
  // tandon terdeteksi habis, walaupun tanah kering.
  if (soilValue < SOIL_THRESHOLD_ON && !cooldownActive && !pumpActive && !waterEmpty) {
    pumpActive = true;
    pumpStart  = now;
    Serial.println("[PUMP] ON — tanah kering (hysteresis ON)");
  } else if (soilValue < SOIL_THRESHOLD_ON && !cooldownActive && !pumpActive && waterEmpty) {
    Serial.println("[PUMP] Tetap OFF — tanah kering TAPI air tandon habis/kritis!");
  } else if (soilValue > SOIL_THRESHOLD_OFF && pumpActive) {
    pumpActive    = false;
    hasEverPumped = true;
    pumpStop      = now;
    Serial.println("[PUMP] OFF — tanah sudah basah (hysteresis OFF)");
  }

  // --- Status kipas (logika hysteresis) ---
  bool pascaSiram = hasEverPumped && !pumpActive &&
                    (now - pumpStop < FAN_AFTER_PUMP);

  bool tempHigh = temperature > TEMP_FAN_ON;
  bool humHigh  = humidity > HUM_FAN_ON;
  bool tempLow  = temperature < TEMP_FAN_OFF;
  bool humLow   = humidity < HUM_FAN_OFF;

  if (pascaSiram || tempHigh || humHigh) {
    fanState = true;
  } else if (tempLow && humLow) {
    fanState = false;
  }
  // else: zona tengah -> tahan state sebelumnya

  if (fanState != fanWasActive) {
    Serial.println(fanState ? "[FAN] ON" : "[FAN] OFF");
  }

  bool pumpChanged = (pumpActive != pumpWasActive);
  bool fanChanged  = (fanState   != fanWasActive);

  // --- Tulis ke pin fisik (verified) + update counter diagnostik ---
  if (pumpChanged) {
    writeRelayVerified(RELAY_PUMP, pumpActive);
    if (pumpActive) {
      pumpCycleCount++;
    } else {
      // Baru saja transisi ke OFF -> tambahkan durasi siklus ini ke total runtime
      pumpTotalRuntimeMs += (pumpStop - pumpStart);
    }
  }

  if (pumpChanged && fanChanged) {
    delay(RELAY_STAGGER_MS); // jeda antar-switch supaya 2 relay tidak aktif persis bersamaan
  }

  if (fanChanged) {
    if (fanState) {
      fanCycleCount++;
      fanOnStart = now;
    } else {
      fanTotalRuntimeMs += (now - fanOnStart);
    }
    writeRelayVerified(RELAY_FAN, fanState);
  }

  Serial.println();
}

// ========== SEND TO FIREBASE (dengan restart jika error) ==========
void sendToFirebase() {
  if (!signupOK || !Firebase.ready()) {
    Serial.println("[Firebase] Tidak siap");
    firebaseFailCount++;
  } else if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Terputus! Mencoba reconnect...");
    WiFi.reconnect();
    firebaseFailCount++;
  } else {
    bool pumpStatus = pumpActive;
    bool fanStatus  = fanState;

    bool ok1 = Firebase.RTDB.setFloat(&fbdo, "/sensor/temperature", temperature);
    bool ok2 = Firebase.RTDB.setFloat(&fbdo, "/sensor/humidity", humidity);
    bool ok3 = Firebase.RTDB.setInt(&fbdo, "/sensor/soilMoisture", soilValue);
    bool ok4 = Firebase.RTDB.setInt(&fbdo, "/sensor/ldrValue", ldrValue);
    bool ok5 = Firebase.RTDB.setFloat(&fbdo, "/sensor/waterLevel", distance);
    bool ok6 = Firebase.RTDB.setBool(&fbdo, "/actuator/pump", pumpStatus);
    bool ok7 = Firebase.RTDB.setBool(&fbdo, "/actuator/fan", fanStatus);
    bool ok8 = Firebase.RTDB.setBool(&fbdo, "/sensor/waterEmpty", waterEmpty);

    if (ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8) {
      Serial.println("[Firebase] Data sensor terkirim.");
      firebaseFailCount = 0;
    } else {
      Serial.println("[Firebase] Gagal kirim: " + fbdo.errorReason());
      firebaseFailCount++;
    }

    // Kirim data diagnostik, best-effort — tidak memengaruhi firebaseFailCount/restart di atas.
    Firebase.RTDB.setInt(&fbdo, "/diagnostic/dhtFaultCount", dhtFaultCount);
    Firebase.RTDB.setInt(&fbdo, "/diagnostic/ultrasonicFaultCount", ultrasonicFaultCount);
    Firebase.RTDB.setInt(&fbdo, "/diagnostic/relayMismatchCount", relayMismatchCount);
    Firebase.RTDB.setInt(&fbdo, "/diagnostic/pumpCycleCount", (int)pumpCycleCount);
    Firebase.RTDB.setInt(&fbdo, "/diagnostic/fanCycleCount", (int)fanCycleCount);
    Firebase.RTDB.setFloat(&fbdo, "/diagnostic/pumpTotalRuntimeMs", (float)pumpTotalRuntimeMs);
    Firebase.RTDB.setFloat(&fbdo, "/diagnostic/fanTotalRuntimeMs", (float)fanTotalRuntimeMs);
    Firebase.RTDB.setInt(&fbdo, "/diagnostic/pumpDryRunStopCount", (int)pumpDryRunStopCount);
  }

  if (firebaseFailCount >= MAX_FB_FAIL) {
    Serial.println("[Firebase] Terlalu banyak error, restart...");
    safeRestart();
  }
}

// ========== SAVE TO HISTORY (dengan retry) ==========
void saveToHistory() {
  if (!signupOK || !Firebase.ready()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  FirebaseJson json;
  json.set("temperature", temperature);
  json.set("humidity",    humidity);
  json.set("soilMoisture", soilValue);
  json.set("ldrValue",    ldrValue);
  json.set("waterLevel",  distance);
  json.set("waterEmpty",  waterEmpty);
  json.set("pump",        pumpActive);
  json.set("fan",         fanState);
  json.set("uptimeMs",    (double)millis());
  // Sertakan diagnostik di riwayat juga, berguna untuk analisis tren fault
  json.set("dhtFaultCount", dhtFaultCount);
  json.set("ultrasonicFaultCount", ultrasonicFaultCount);
  json.set("relayMismatchCount", relayMismatchCount);
  json.set("pumpDryRunStopCount", (int)pumpDryRunStopCount);

  if (Firebase.RTDB.pushJSON(&fbdo, "/history", &json)) {
    Serial.println("[Firebase] History tersimpan: /history/" + fbdo.pushName());
  } else {
    Serial.println("[Firebase] Gagal simpan history: " + fbdo.errorReason());
  }
}

// ========== MAIN LOOP ==========
void loop() {
  esp_task_wdt_reset();

  unsigned long now = millis();

  if (now - lastSensor >= SENSOR_INTERVAL) {
    lastSensor = now;
    readSensors();
    controlActuators();
  }

  if (now - lastFirebase >= FIREBASE_INTERVAL) {
    lastFirebase = now;
    sendToFirebase();
  }

  if (now - lastHistory >= HISTORY_INTERVAL) {
    lastHistory = now;
    saveToHistory();
  }
}
