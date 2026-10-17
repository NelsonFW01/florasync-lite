#include <DHT.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <esp_task_wdt.h>   // watchdog timer, restart otomatis kalau loop() macet
#include "esp_system.h"     // untuk esp_reset_reason()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"  // mutex antar-task

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
// FirebaseData terpisah khusus untuk /history. Firebase-ESP32 client bersifat
// blocking/sinkron per FirebaseData object -- kalau history (event-based,
// bisa terpicu kapan saja saat status berubah) berbagi object yang sama
// dengan sendToFirebase() (rutin per FIREBASE_INTERVAL), satu bisa
// menimpa/menunggu state buffer yang lain. Dengan object terpisah,
// write ke /sensor+/actuator dan write ke /history tidak saling
// mengganggu walau waktunya berdekatan.
FirebaseData fbdoHistory;
FirebaseAuth auth;
FirebaseConfig config;

// ========== PUMP CONTROL (dengan HYSTERESIS) ==========
const int SOIL_THRESHOLD_ON  = 3100;   // tanah kering → nyalakan pompa (hasil kalibrasi tes terpisah)
const int SOIL_THRESHOLD_OFF = 3000;   // tanah basah → matikan pompa (hasil kalibrasi tes terpisah)
const unsigned long PUMP_DURATION  = 30000;  // 30 detik
const unsigned long PUMP_COOLDOWN  = 60000;  // 1 menit
const unsigned long FAN_AFTER_PUMP = 90000;  // sudah tidak dipakai lagi (kipas sekarang independen dari pompa) - dibiarkan agar tidak perlu ubah bagian lain

bool pumpActive    = false;
bool hasEverPumped = false;
unsigned long pumpStart = 0;
unsigned long pumpStop  = 0;

// ========== HISTORY EVENT QUEUE ==========
// Event aktuator disimpan sebagai snapshot saat perubahan benar-benar terjadi.
// Ini mencegah event hilang ketika Firebase sedang sibuk / koneksi lambat.
struct HistoryEvent {
  float temperature;
  float humidity;
  int soilValue;
  int ldrValue;
  float distance;
  bool waterEmpty;
  bool pump;
  bool fan;
  unsigned long uptimeMs;
  int dhtFaultCount;
  int ultrasonicFaultCount;
  int relayMismatchCount;
  int pumpDryRunStopCount;
};

const int HISTORY_QUEUE_SIZE = 20;
HistoryEvent historyQueue[HISTORY_QUEUE_SIZE];
int historyQueueHead = 0;
int historyQueueTail = 0;
int historyQueueCount = 0;
unsigned long historyDroppedCount = 0;

// ========== FAN CONTROL (dengan HYSTERESIS, INDEPENDEN dari pompa) ==
const float TEMP_FAN_ON   = 31.0;   // kipas ON jika suhu > 31°C
const float TEMP_FAN_OFF  = 29.0;   // kipas OFF jika suhu < 29°C
const float HUM_FAN_ON    = 82.0;   // kipas ON jika RH > 82%
const float HUM_FAN_OFF   = 78.0;   // kipas OFF jika RH < 78%
bool fanState = false;

// ========== TIMING ==========
const unsigned long SENSOR_INTERVAL   = 5000;
const unsigned long FIREBASE_INTERVAL = 5000;
const unsigned long HISTORY_INTERVAL  = 60000;
// Lapis proteksi kedua terhadap boot-loop: jangan proses perintah restart
// jarak jauh dalam N detik pertama sejak device baru nyala. Sengaja diam
// dulu di awal biar koneksi WiFi/Firebase benar-benar stabil sebelum mulai
// baca-tulis flag /commands/restart.
const unsigned long BOOT_GRACE_PERIOD_MS = 30000; // 30 detik

unsigned long lastFirebase = 0;
unsigned long lastPeriodicHistory = 0;
unsigned long lastDiagnostic = 0;
const unsigned long DIAGNOSTIC_INTERVAL = 60000;

// Forward declarations agar aman baik di .ino maupun .cpp.
void enqueueHistoryEvent();
bool saveHistoryEvent(const HistoryEvent &event, const char* reason);
bool saveCurrentHistory(const char* reason);
void sendToFirebase();

bool signupOK = false;

// ========== MULTITASKING (dual-core FreeRTOS) ==========
// ESP32 punya 2 core fisik. Task A (baca sensor + kontrol aktuator) jalan
// di Core 1, Task B (komunikasi Firebase, yang paling banyak nunggu
// jaringan) jalan di Core 0 - dua-duanya bisa jalan BERSAMAAN secara
// fisik, bukan cuma bergantian macam sebelumnya (single-thread loop()).
//
// dataMutex WAJIB dipegang setiap kali baca/tulis variabel yang dipakai
// KEDUA task (temperature, humidity, soilValue, ldrValue, distance,
// waterEmpty, pumpActive, fanState, actuatorChangedThisCycle, dan semua
// counter diagnostik). Task B (Firebase) cuma pegang mutex SEBENTAR untuk
// nyalin nilai-nilai itu ke variabel lokal, lalu langsung lepas mutex
// SEBELUM melakukan panggilan jaringan yang lambat - supaya Task A (baca
// sensor) tidak ikut ketahan nunggu Firebase.
SemaphoreHandle_t dataMutex;
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t firebaseTaskHandle = NULL;

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
// WATER_LEVEL_OK_CM = jarak sensor ke air saat tinggi air minimum yang
// diizinkan (0,5 cm dari dasar) -> 18,5 - 0,5 = 18,0 cm.
// Kalau batas amannya mau diganti nanti: WATER_LEVEL_OK_CM =
// WATER_LEVEL_EMPTY_CM dikurangi tinggi air minimum yang diinginkan (cm).
//
// !! PERINGATAN: zona hysteresis di sini cuma 0,5 cm (18,0 - 18,5), jauh
// lebih sempit dari sebelumnya (dulu 6 cm). HC-SR04 punya margin error
// alami sekitar ±0,3 cm (bisa lebih di tandon kecil karena pantulan
// berulang di dinding) - jadi ada risiko status waterEmpty "kedap-kedip"
// ON/OFF cuma karena noise pembacaan, bukan airnya beneran berubah. Kalau
// itu kejadian, lebarkan lagi WATER_LEVEL_OK_CM (mis. jadi 17.5 = 1 cm).
const float WATER_LEVEL_EMPTY_CM = 18.5;
const float WATER_LEVEL_OK_CM    = 18.0;
const int   MAX_ULTRASONIC_FAULT_TRUST = 3; 

bool  waterEmpty        = true;  // default aman saat boot: anggap habis sampai terbukti ada air
float lastValidDistance = -1;

// ========== ERROR COUNTER UNTUK RESTART ==========
int wifiFailCount = 0;      // dipakai buat log & jadwal WiFi.reconnect() - TIDAK memicu restart lagi
int firebaseFailCount = 0;  // dipakai buat log periodik saja - TIDAK memicu restart lagi

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
// isBrownoutBoot & lastResetReasonStr dipakai lagi nanti di
// reportResetReasonToFirebase() supaya kejadian brownout kelihatan di
// dashboard, tidak cuma di Serial Monitor.
bool   isBrownoutBoot   = false;
String lastResetReasonStr = "unknown";

void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("[BOOT] Alasan restart terakhir: ");
  switch (reason) {
    case ESP_RST_POWERON:
      lastResetReasonStr = "poweron";
      Serial.println("Power-on normal (colok listrik / reset manual)");
      break;
    case ESP_RST_EXT:
      lastResetReasonStr = "external";
      Serial.println("Reset via pin eksternal");
      break;
    case ESP_RST_SW:
      lastResetReasonStr = "software";
      Serial.println("Software reset (ESP.restart() dipanggil oleh kode)");
      break;
    case ESP_RST_PANIC:
      lastResetReasonStr = "panic";
      Serial.println("PANIC/crash pada kode (exception)");
      break;
    case ESP_RST_INT_WDT:
      lastResetReasonStr = "interrupt_watchdog";
      Serial.println("Interrupt watchdog timeout");
      break;
    case ESP_RST_TASK_WDT:
      lastResetReasonStr = "task_watchdog";
      Serial.println("Task watchdog timeout (loop() sempat macet)");
      break;
    case ESP_RST_WDT:
      lastResetReasonStr = "watchdog";
      Serial.println("Watchdog lain timeout");
      break;
    case ESP_RST_BROWNOUT:
      lastResetReasonStr = "brownout";
      isBrownoutBoot = true;
      Serial.println("!!! BROWNOUT — tegangan sempat drop di bawah batas aman !!!");
      Serial.println("     -> Kemungkinan besar power supply kurang kuat saat");
      Serial.println("        pompa & kipas (atau bebannya) narik arus bersamaan.");
      break;
    case ESP_RST_DEEPSLEEP:
      lastResetReasonStr = "deepsleep";
      Serial.println("Bangun dari deep sleep");
      break;
    default:
      lastResetReasonStr = "unknown";
      Serial.print("Lainnya/tidak diketahui (kode=");
      Serial.print((int)reason);
      Serial.println(")");
      break;
  }
}

// ---------- Laporkan alasan restart ke Firebase ----------
// Dipanggil sekali di setup() setelah Firebase siap. Menulis 2 hal:
// 1. /diagnostics/lastReset  -> snapshot terbaru saja (gampang dibaca dashboard)
// 2. /diagnostics/resetLog   -> riwayat tiap boot (push, jadi ada timeline-nya)
// Kalau ada brownout SEBELUM boot terbaru ini, "isBrownoutBoot" nyala TRUE
// hanya untuk boot yang bersangkutan (per definisi esp_reset_reason() ESP32),
// jadi tiap entri di resetLog memang catatan boot itu sendiri, bukan akumulasi.
void reportResetReasonToFirebase() {
  if (!signupOK || !Firebase.ready()) return;

  FirebaseJson snap;
  snap.set("reason",     lastResetReasonStr);
  snap.set("isBrownout", isBrownoutBoot);
  Firebase.RTDB.setJSON(&fbdo, "/diagnostics/lastReset", &snap);

  FirebaseJson log;
  log.set("reason",     lastResetReasonStr);
  log.set("isBrownout", isBrownoutBoot);
  if (Firebase.RTDB.pushJSON(&fbdo, "/diagnostics/resetLog", &log)) {
    Serial.println("[Firebase] Reset-reason terkirim: " + lastResetReasonStr);
  } else {
    Serial.println("[Firebase] Gagal kirim reset-reason: " + fbdo.errorReason());
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
    // PENTING: dulu di sini langsung safeRestart() kalau WiFi gagal —
    // akibatnya kalau WiFi rumah lagi mati (mis. router dimatikan malam
    // hari), ESP32 boot-loop TERUS-MENERUS dan sensorControlTask (yang
    // ngontrol pompa/kipas) TIDAK PERNAH dibuat sama sekali. Padahal
    // kontrol pompa/kipas itu murni logika LOKAL, nggak butuh internet.
    // Sekarang: kalau WiFi gagal, cuma dicatat & lanjut boot APA ADANYA.
    // Firebase/dashboard nggak akan update selama offline, tapi pompa &
    // kipas TETAP jalan normal berdasarkan sensor lokal. WiFi.reconnect()
    // tetap dicoba periodik di background (lihat sensorControlTask).
    Serial.println("\nWiFi belum konek. Lanjut boot OFFLINE — pompa/kipas");
    Serial.println("tetap jalan dari sensor lokal, dashboard nunggu WiFi balik.");
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

  // Tunggu sebentar sampai Firebase auth siap (token didapat), lalu kirim
  // alasan restart. Dibatasi 10 detik supaya tidak nge-hang kalau auth
  // lambat/gagal - fitur ini opsional, bukan boleh sampai menunda boot.
  {
    int fbRetries = 0;
    while (!Firebase.ready() && fbRetries < 20) {
      delay(500);
      fbRetries++;
    }
    reportResetReasonToFirebase();
  }

  // ========== SIAPKAN MUTEX & JALANKAN TASK DUAL-CORE ==========
  // Watchdog per-task (bukan lagi 1 watchdog untuk loop() tunggal) -
  // masing-masing task daftarkan dirinya sendiri di awal fungsinya.
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 600000,    // 10 menit timeout
    .trigger_panic = true    // restart otomatis jika hang
  };
  esp_err_t twdtErr = esp_task_wdt_init(&twdt_config);
  if (twdtErr == ESP_ERR_INVALID_STATE) {
    // PENTING: Arduino-ESP32 core versi baru sudah otomatis mengaktifkan
    // task watchdog SEBELUM setup() kita jalan, biasanya dengan timeout
    // default yang pendek (~5 detik). Panggilan esp_task_wdt_init() di atas
    // jadi GAGAL DIAM-DIAM (nggak error fatal, cuma log "already
    // initialized") dan config 600 detik kita TIDAK PERNAH benar-benar
    // terpasang - watchdog yang aktif tetap yang default 5 detik. Ini
    // yang bikin device panic-restart kalau firebaseTask kebetulan nunggu
    // koneksi TLS agak lama (>5 detik). Fix: deinit dulu watchdog default
    // itu, baru init ulang pakai config kita yang benar.
    Serial.println("[BOOT] TWDT default sudah aktif, reinit dengan timeout 10 menit...");
    esp_task_wdt_deinit();
    esp_task_wdt_init(&twdt_config);
  }

  dataMutex = xSemaphoreCreateMutex();
  if (dataMutex == NULL) {
    Serial.println("[FATAL] Gagal membuat mutex! Restart...");
    safeRestart();
  }

  // Core 1: baca sensor + kontrol aktuator (kerja lokal, tidak nunggu jaringan)
  xTaskCreatePinnedToCore(
    sensorControlTask,
    "SensorControl",
    6144,           // stack (bytes) - cukup untuk DHT22 + pembacaan ADC
    NULL,
    1,              // priority
    &sensorTaskHandle,
    1               // Core 1
  );

  // Core 0: komunikasi Firebase (banyak nunggu TLS/jaringan, jangan sampai
  // ikut menahan pembacaan sensor kalau internet lambat)
  xTaskCreatePinnedToCore(
    firebaseTask,
    "FirebaseComm",
    10240,          // stack (bytes) - Firebase client + TLS butuh lebih besar
    NULL,
    1,              // priority
    &firebaseTaskHandle,
    0               // Core 0
  );

  Serial.println("FloraSync Lite siap. (dual-core: Core1=sensor/aktuator, Core0=Firebase)");
  Serial.println("===================");
}

// ========== TASK CORE 1: SENSOR + AKTUATOR ==========
void sensorControlTask(void *pvParameters) {
  esp_task_wdt_add(NULL); // daftarkan TASK INI (bukan loop utama) ke watchdog
  for (;;) {
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
      readSensors();
      controlActuators();
      xSemaphoreGive(dataMutex);
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL));
  }
}

// ========== TASK CORE 0: FIREBASE (STATUS RUTIN + HISTORY) ==========
void firebaseTask(void *pvParameters) {
  esp_task_wdt_add(NULL); // daftarkan TASK INI ke watchdog
  unsigned long lastRestartCheck = 0;
  const unsigned long RESTART_CHECK_INTERVAL = 5000; // cek perintah restart tiap 5 detik

  for (;;) {
    unsigned long now = millis();

    if (now - lastFirebase >= FIREBASE_INTERVAL) {
      lastFirebase = now;
      sendToFirebase();
    }

    // --- Perintah restart jarak jauh: dashboard nulis /commands/restart=true,
    // device baca berkala di sini.
    // PENTING (bug yang pernah kejadian): kalau setBool(...,false) di bawah
    // GAGAL (mis. koneksi sempat goyang), device dulu TETAP restart walau
    // flag-nya gagal kehapus -> boot lagi -> baca flag masih true -> restart
    // lagi -> BOOT-LOOP TANPA HENTI. Sekarang restart CUMA dieksekusi kalau
    // penghapusan flag-nya kebukti BERHASIL (dicek return value-nya). Kalau
    // gagal, coba lagi 5 detik berikutnya - nggak restart dulu sampai
    // beneran kehapus, jadi nggak akan pernah loop tanpa henti. ---
    if (signupOK && Firebase.ready() && WiFi.status() == WL_CONNECTED &&
        (millis() > BOOT_GRACE_PERIOD_MS) && // jangan proses perintah restart di detik-detik awal boot
        (now - lastRestartCheck >= RESTART_CHECK_INTERVAL)) {
      lastRestartCheck = now;
      if (Firebase.RTDB.getBool(&fbdo, "/commands/restart")) {
        if (fbdo.dataType() == "boolean" && fbdo.boolData()) {
          Serial.println("[Firebase] Perintah restart diterima dari dashboard.");
          bool cleared = Firebase.RTDB.setBool(&fbdo, "/commands/restart", false);
          if (cleared) {
            delay(300); // beri waktu write flag-reset itu sendiri terkirim
            safeRestart();
          } else {
            Serial.println("[Firebase] Gagal hapus flag restart - coba lagi siklus berikutnya, TIDAK restart dulu.");
          }
        }
      }
    }

    // --- EVENT HISTORY ---
    // Ambil event paling lama dari queue. Event TIDAK dihapus sampai
    // pushJSON berhasil, sehingga event tidak hilang saat Firebase gagal.
    HistoryEvent pendingEvent;
    bool hasPendingEvent = false;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (historyQueueCount > 0) {
        pendingEvent = historyQueue[historyQueueHead];
        hasPendingEvent = true;
      }
      xSemaphoreGive(dataMutex);
    }

    if (hasPendingEvent) {
      if (saveHistoryEvent(pendingEvent, "actuatorChange")) {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
          // Hapus hanya jika event yang masih di head adalah event yang tadi
          // berhasil dikirim.
          if (historyQueueCount > 0 &&
              historyQueue[historyQueueHead].uptimeMs == pendingEvent.uptimeMs) {
            historyQueueHead = (historyQueueHead + 1) % HISTORY_QUEUE_SIZE;
            historyQueueCount--;
          }
          xSemaphoreGive(dataMutex);
        }
      }
    }

    // --- PERIODIK ---
    // Timer periodik TERPISAH dari event history. Jadi event pada detik ke-30
    // tidak lagi menggeser jadwal snapshot 60 detik berikutnya.
    if (now - lastPeriodicHistory >= HISTORY_INTERVAL) {
      if (saveCurrentHistory("periodic")) {
        lastPeriodicHistory = now;
      }
    }

    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(200)); // cek tiap 200ms - cukup responsif utk event-based, ringan buat CPU
  }
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
    Serial.printf("[WiFi] Terputus (%d siklus). Pompa/kipas tetap jalan (lokal).\n", wifiFailCount);
    // Nggak restart di sini lagi — pompa/kipas nggak butuh WiFi buat jalan.
    // Cukup coba reconnect pelan-pelan di background; begitu WiFi balik,
    // sendToFirebase() otomatis lanjut ngirim data lagi tanpa perlu reboot.
    if (wifiFailCount % 6 == 0) { // coba reconnect tiap ~30 detik, jangan spam
      WiFi.reconnect();
    }
  } else {
    if (wifiFailCount > 0) {
      Serial.println("[WiFi] Tersambung kembali.");
    }
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
    Serial.println("[PUMP] OFF — timer 30 detik selesai"); // selaras dengan PUMP_DURATION = 30000
  }

  bool cooldownActive = hasEverPumped && !pumpActive &&
                        (now - pumpStop < PUMP_COOLDOWN);

  // Syarat tambahan !waterEmpty: pompa tidak boleh menyala kalau air
  // tandon terdeteksi habis, walaupun tanah kering.
  if (soilValue > SOIL_THRESHOLD_ON && !cooldownActive && !pumpActive && !waterEmpty) {
    pumpActive = true;
    pumpStart  = now;
    Serial.println("[PUMP] ON — tanah kering (hysteresis ON)");
  } else if (soilValue > SOIL_THRESHOLD_ON && !cooldownActive && !pumpActive && waterEmpty) {
    Serial.println("[PUMP] Tetap OFF — tanah kering TAPI air tandon habis/kritis!");
  } else if (soilValue < SOIL_THRESHOLD_OFF && pumpActive) {
    pumpActive    = false;
    hasEverPumped = true;
    pumpStop      = now;
    Serial.println("[PUMP] OFF — tanah sudah basah (hysteresis OFF)");
  }

  // --- Status kipas (INDEPENDEN dari pompa, murni suhu & kelembaban) ---
  bool tempHigh = temperature > TEMP_FAN_ON;
  bool humHigh  = humidity > HUM_FAN_ON;
  bool tempLow  = temperature < TEMP_FAN_OFF;
  bool humLow   = humidity < HUM_FAN_OFF;

  if (tempHigh || humHigh) {
    fanState = true;
  } else if (tempLow && humLow) {
    fanState = false;
  }
  // else: zona hysteresis tengah -> pertahankan status kipas sebelumnya

  if (fanState != fanWasActive) {
    Serial.println(fanState ? "[FAN] ON" : "[FAN] OFF");
  }

  bool pumpChanged = (pumpActive != pumpWasActive);
  bool fanChanged  = (fanState   != fanWasActive);

  // --- Tulis ke pin fisik ---
  // PENTING: jika pompa dan kipas berubah pada siklus yang sama,
  // kedua GPIO ditulis langsung pada bagian yang sama tanpa stagger.
  // Dengan begitu keduanya dapat ON bersamaan, sementara keputusan ON/OFF
  // tetap ditentukan oleh sensor masing-masing.
  if (pumpChanged) {
    if (pumpActive) {
      pumpCycleCount++;
    } else {
      // Baru saja transisi ke OFF -> tambahkan durasi siklus ini ke total runtime
      pumpTotalRuntimeMs += (pumpStop - pumpStart);
    }
  }

  if (fanChanged) {
    if (fanState) {
      fanCycleCount++;
      fanOnStart = now;
    } else {
      fanTotalRuntimeMs += (now - fanOnStart);
    }
  }

  // Tulis keadaan KEDUA relay pada siklus yang sama.
  // Active-LOW: LOW = ON, HIGH = OFF.
  if (pumpChanged || fanChanged) {
    digitalWrite(RELAY_PUMP, pumpActive ? LOW : HIGH);
    digitalWrite(RELAY_FAN,  fanState   ? LOW : HIGH);

    // Verifikasi level GPIO setelah kedua pin ditulis.
    int expectedPumpLevel = pumpActive ? LOW : HIGH;
    int expectedFanLevel  = fanState   ? LOW : HIGH;

    if (digitalRead(RELAY_PUMP) != expectedPumpLevel) {
      relayMismatchCount++;
      Serial.println("[RELAY] WARNING: GPIO22 (PUMP) tidak sesuai target!");
    }

    if (digitalRead(RELAY_FAN) != expectedFanLevel) {
      relayMismatchCount++;
      Serial.println("[RELAY] WARNING: GPIO23 (FAN) tidak sesuai target!");
    }

    Serial.printf("[RELAY] PUMP=%s | FAN=%s\n",
                  pumpActive ? "ON" : "OFF",
                  fanState   ? "ON" : "OFF");
  }

  // Simpan snapshot EVENT saat perubahan benar-benar terjadi.
  // Snapshot masuk queue sebelum fungsi selesai, sehingga event tidak hilang
  // walaupun Firebase sedang lambat.
  if (pumpChanged || fanChanged) {
    enqueueHistoryEvent();
  }

  Serial.println();
}

// ========== SEND TO FIREBASE (dengan restart jika error) ==========
void sendToFirebase() {
  // --- Snapshot cepat semua data yang dibagi dengan sensorControlTask,
  // lalu LEPAS mutex sebelum melakukan panggilan jaringan yang lambat. ---
  float t, h, dist;
  int soil, ldr;
  bool pumpStatus, fanStatus, empty;
  int dhtF, ultraF, relayMis;
  unsigned long pumpCyc, fanCyc, dryRun;
  float pumpRt, fanRt;

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    t = temperature; h = humidity; dist = distance;
    soil = soilValue; ldr = ldrValue;
    pumpStatus = pumpActive; fanStatus = fanState; empty = waterEmpty;
    dhtF = dhtFaultCount; ultraF = ultrasonicFaultCount; relayMis = relayMismatchCount;
    pumpCyc = pumpCycleCount; fanCyc = fanCycleCount; dryRun = pumpDryRunStopCount;
    pumpRt = (float)pumpTotalRuntimeMs; fanRt = (float)fanTotalRuntimeMs;
    xSemaphoreGive(dataMutex);
  } else {
    Serial.println("[Firebase] Gagal ambil data (mutex timeout), skip siklus ini");
    return;
  }

  if (!signupOK || !Firebase.ready()) {
    Serial.println("[Firebase] Tidak siap");
    firebaseFailCount++;
  } else if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Terputus! Mencoba reconnect...");
    WiFi.reconnect();
    firebaseFailCount++;
  } else {
    // Sebelumnya 8 write terpisah. Sekarang sensor + status aktuator
    // dikirim sebagai 2 updateNode agar waktu blocking jaringan jauh lebih kecil.
    FirebaseJson sensorJson;
    sensorJson.set("temperature", t);
    sensorJson.set("humidity", h);
    sensorJson.set("soilMoisture", soil);
    sensorJson.set("ldrValue", ldr);
    sensorJson.set("waterLevel", dist);
    sensorJson.set("waterEmpty", empty);

    FirebaseJson actuatorJson;
    actuatorJson.set("pump", pumpStatus);
    actuatorJson.set("fan", fanStatus);

    bool okSensor = Firebase.RTDB.updateNode(&fbdo, "/sensor", &sensorJson);
    bool okActuator = Firebase.RTDB.updateNode(&fbdo, "/actuator", &actuatorJson);

    if (okSensor && okActuator) {
      Serial.println("[Firebase] Sensor + actuator terkirim.");
      firebaseFailCount = 0;
    } else {
      Serial.println("[Firebase] Gagal kirim sensor/actuator: " + fbdo.errorReason());
      firebaseFailCount++;
    }

    // Diagnostic tidak perlu dikirim setiap 5 detik.
    // Kirim maksimal 1x per menit agar jumlah request Firebase ringan.
    unsigned long now = millis();
    if (now - lastDiagnostic >= DIAGNOSTIC_INTERVAL) {
      FirebaseJson diagnosticJson;
      diagnosticJson.set("dhtFaultCount", dhtF);
      diagnosticJson.set("ultrasonicFaultCount", ultraF);
      diagnosticJson.set("relayMismatchCount", relayMis);
      diagnosticJson.set("pumpCycleCount", (int)pumpCyc);
      diagnosticJson.set("fanCycleCount", (int)fanCyc);
      diagnosticJson.set("pumpTotalRuntimeMs", pumpRt);
      diagnosticJson.set("fanTotalRuntimeMs", fanRt);
      diagnosticJson.set("pumpDryRunStopCount", (int)dryRun);
      diagnosticJson.set("historyDroppedCount", (int)historyDroppedCount); // TAMBAHAN: supaya kelihatan di dashboard kalau antrean pernah penuh/buang event

      if (Firebase.RTDB.updateNode(&fbdo, "/diagnostic", &diagnosticJson)) {
        lastDiagnostic = now;
      }
    }
  }

  // Catatan: dulu di sini ada safeRestart() kalau firebaseFailCount terlalu
  // tinggi. Itu ikut jadi penyebab pompa/kipas berhenti tiap kali WiFi lama
  // putus (mis. malam hari router mati) — device kena reboot berulang-ulang
  // padahal kontrol pompa/kipas nggak butuh Firebase sama sekali. Sekarang
  // dibiarkan gagal terus mencatat log doang; begitu WiFi/Firebase balik,
  // sendToFirebase() otomatis lanjut normal tanpa perlu reboot device.
  if (firebaseFailCount > 0 && firebaseFailCount % 20 == 0) {
    Serial.printf("[Firebase] Masih gagal setelah %d percobaan (device tetap jalan offline).\n", firebaseFailCount);
  }
}

// ========== HISTORY HELPERS ==========

// Masukkan snapshot event ke queue.
// Fungsi ini dipanggil dari sensor task ketika mutex data sedang dipegang.
void enqueueHistoryEvent() {
  HistoryEvent event;

  event.temperature = temperature;
  event.humidity = humidity;
  event.soilValue = soilValue;
  event.ldrValue = ldrValue;
  event.distance = distance;
  event.waterEmpty = waterEmpty;
  event.pump = pumpActive;
  event.fan = fanState;
  event.uptimeMs = millis();
  event.dhtFaultCount = dhtFaultCount;
  event.ultrasonicFaultCount = ultrasonicFaultCount;
  event.relayMismatchCount = relayMismatchCount;
  event.pumpDryRunStopCount = pumpDryRunStopCount;

  if (historyQueueCount >= HISTORY_QUEUE_SIZE) {
    // Buang event paling lama agar event terbaru tetap tercatat.
    historyQueueHead = (historyQueueHead + 1) % HISTORY_QUEUE_SIZE;
    historyQueueCount--;
    historyDroppedCount++;
  }

  historyQueue[historyQueueTail] = event;
  historyQueueTail = (historyQueueTail + 1) % HISTORY_QUEUE_SIZE;
  historyQueueCount++;
}

// Kirim event snapshot yang sudah terjadi.
// Event baru DIHAPUS dari queue hanya jika fungsi ini mengembalikan true.
bool saveHistoryEvent(const HistoryEvent &event, const char* reason) {
  if (!signupOK || !Firebase.ready()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  FirebaseJson json;
  json.set("temperature", event.temperature);
  json.set("humidity", event.humidity);
  json.set("soilMoisture", event.soilValue);
  json.set("ldrValue", event.ldrValue);
  json.set("waterLevel", event.distance);
  json.set("waterEmpty", event.waterEmpty);
  json.set("pump", event.pump);
  json.set("fan", event.fan);
  json.set("trigger", reason);
  json.set("uptimeMs", (double)event.uptimeMs);
  json.set("dhtFaultCount", event.dhtFaultCount);
  json.set("ultrasonicFaultCount", event.ultrasonicFaultCount);
  json.set("relayMismatchCount", event.relayMismatchCount);
  json.set("pumpDryRunStopCount", event.pumpDryRunStopCount);

  if (Firebase.RTDB.pushJSON(&fbdoHistory, "/history", &json)) {
    Serial.printf("[Firebase] History event tersimpan: /history/%s\n",
                  fbdoHistory.pushName().c_str());
    return true;
  }

  Serial.println("[Firebase] Gagal simpan history event: " + fbdoHistory.errorReason());
  return false;
}

// Snapshot kondisi TERKINI untuk history periodik.
// Timer periodik tidak dipengaruhi oleh event actuator.
bool saveCurrentHistory(const char* reason) {
  if (!signupOK || !Firebase.ready()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  float t, h, dist;
  int soil, ldr;
  bool pumpStatus, fanStatus, empty;
  int dhtF, ultraF, relayMis;
  unsigned long dryRun;

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    t = temperature;
    h = humidity;
    soil = soilValue;
    ldr = ldrValue;
    dist = distance;
    pumpStatus = pumpActive;
    fanStatus = fanState;
    empty = waterEmpty;
    dhtF = dhtFaultCount;
    ultraF = ultrasonicFaultCount;
    relayMis = relayMismatchCount;
    dryRun = pumpDryRunStopCount;
    xSemaphoreGive(dataMutex);
  } else {
    Serial.println("[Firebase] Gagal ambil data untuk history periodik.");
    return false;
  }

  FirebaseJson json;
  json.set("temperature", t);
  json.set("humidity", h);
  json.set("soilMoisture", soil);
  json.set("ldrValue", ldr);
  json.set("waterLevel", dist);
  json.set("waterEmpty", empty);
  json.set("pump", pumpStatus);
  json.set("fan", fanStatus);
  json.set("trigger", reason);
  json.set("uptimeMs", (double)millis());
  json.set("dhtFaultCount", dhtF);
  json.set("ultrasonicFaultCount", ultraF);
  json.set("relayMismatchCount", relayMis);
  json.set("pumpDryRunStopCount", (int)dryRun);

  if (Firebase.RTDB.pushJSON(&fbdoHistory, "/history", &json)) {
    Serial.printf("[Firebase] History periodik tersimpan: /history/%s\n",
                  fbdoHistory.pushName().c_str());
    return true;
  }

  Serial.println("[Firebase] Gagal simpan history periodik: " + fbdoHistory.errorReason());
  return false;
}

// ========== MAIN LOOP ==========
// Sengaja dibiarkan (hampir) kosong — semua kerjaan nyata sekarang jalan di
// sensorControlTask (Core 1) dan firebaseTask (Core 0), yang dibuat di
// akhir setup(). loop() ini sendiri jalan di task bawaan Arduino
// ("loopTask", biasanya juga di Core 1) tapi tidak melakukan apa-apa yang
// signifikan lagi, cuma tidur supaya tidak menghabiskan waktu CPU sia-sia.
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}