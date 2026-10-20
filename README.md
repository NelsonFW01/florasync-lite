# 🌱 FloraSync Lite

> **Greenhouse Portable IoT Skala Mikro Hemat Energi untuk Budidaya Tanaman Hortikultura**
>
> Skripsi — Program Studi Teknik Informatika, Universitas Bina Nusantara, 2026

---

## 📖 Tentang Proyek

FloraSync Lite adalah sistem greenhouse portabel skala mikro berbasis Internet of Things (IoT) yang dirancang untuk mendukung kegiatan urban farming di lingkungan perkotaan. Sistem ini memungkinkan pemantauan dan pengendalian kondisi lingkungan tanaman secara real-time melalui aplikasi web, dengan pendekatan desain daya rendah (*low-power design*) menggunakan adaptor 5V/2A.

**Dimensi greenhouse:** 55 × 45 × 40 cm  
**Tanaman:** Jeruk Limau, Cabe Rawit, Tomat Cherry  
**Mikrokontroler:** ESP32  

---

## 🏗️ Arsitektur Sistem

```
Sensor (DHT22, Soil Moisture, LDR, HC-SR04)
        ↓
    ESP32 (Controller + WiFi)
        ↓
Firebase Realtime Database (Cloud)
        ↓
Web App Dashboard (HTML/CSS/JS)
        ↓
    Pengguna (Browser / Smartphone)
```

---

## 🔧 Hardware yang Digunakan

| Komponen | Fungsi |
|---|---|
| ESP32 | Mikrokontroler utama + koneksi WiFi |
| DHT22 | Sensor suhu dan kelembaban udara |
| Capacitive Soil Moisture Sensor | Sensor kelembaban tanah |
| LDR Light Sensor | Sensor intensitas cahaya |
| HC-SR04 Ultrasonic | Sensor ketinggian air reservoir |
| Relay Module 5V 4-Channel | Pengendali aktuator |
| Mini Pump 5V | Pompa air otomatis |
| Mini Fan 5V | Kipas sirkulasi udara |
| Breadboard + Jumper Wire | Media rangkaian prototipe |
| Adaptor 5V/2A | Sumber daya sistem |

---

## 💻 Fitur Web App

| Halaman | Fitur |
|---|---|
| **Login** | Autentikasi dengan email & password via Firebase Auth |
| **Dashboard** | Ringkasan kondisi sensor (dalam kata sehari-hari) & status aktuator real-time |
| **Monitoring** | Kondisi sensor detail + grafik tren, **Batas Otomatisasi**, dan arti kondisi sensor |
| **Data History** | Riwayat data dengan filter rentang tanggal (kondisi + angka ADC) |
| **Export Data** | Download data dalam format CSV atau PDF (kondisi + angka ADC) |

### Bahasa yang ramah pengguna

| Sensor | Yang ditampilkan | Angka mentah |
|---|---|---|
| Kelembaban tanah | **Kering / Lembap / Basah** | tetap tampil kecil sebagai "Nilai sensor: … ADC" (dan kolom terpisah di CSV/PDF) |
| Intensitas cahaya | **Rendah / Cukup / Tinggi** | sama |
| Level air tandon | **Cukup / Menipis / Habis** | jarak sensor ke permukaan air (cm) |

Batas tiap kategori **tidak ditulis ulang di web**: ESP32 mengirimkannya ke Firebase (`/config`) setiap kali menyala, sehingga web selalu memakai angka kalibrasi yang sama dengan alat. Kalau `/config` belum ada, web memakai nilai bawaan yang sama dengan firmware.

### Status perangkat (Dashboard & Monitoring konsisten)

Kedua halaman memakai satu logika yang sama (`createDeviceMonitor` di `shared.js`):

| Status | Artinya | Tampilan pompa & kipas |
|---|---|---|
| **Memeriksa koneksi…** | Baru membuka halaman, maksimal 12 detik | "Memeriksa…" |
| **Perangkat terhubung** | Data dari ESP32 masuk dalam 20 detik terakhir | **Menyala / Mati** |
| **Perangkat terputus** | Tidak ada data baru > 20 detik (atau tidak ada data sama sekali setelah 12 detik) | **Offline** + banner penjelasan; angka sensor diberi keterangan "nilai terakhir" |

Penentuan online/offline memakai *heartbeat* `/sensor/lastSeen` (timestamp server Firebase, ditulis ESP32 tiap 5 detik) yang dibandingkan dengan `.info/serverTimeOffset`, sehingga tidak bergantung pada jam HP/laptop. Untuk firmware lama yang belum mengirim `lastSeen`, web otomatis memakai cara cadangan (menunggu pembaruan data berikutnya).

---

## ⚙️ Logika Kontrol Otomatis ESP32

Angka berikut diambil dari `florasync_firmware.ino` dan ditampilkan di Monitoring → **Batas Otomatisasi**.

| Kondisi | Aksi |
|---|---|
| Kelembaban tanah > 3100 ADC (tanah **kering**) | Pompa menyala selama 30 detik |
| Kelembaban tanah < 3000 ADC (tanah **basah**) | Pompa berhenti lebih awal |
| 3000 – 3100 ADC (tanah **lembap**) | Zona hysteresis: tidak mengubah status pompa |
| Jeda antarpenyiraman | 60 detik |
| Suhu udara > 31°C | Kipas menyala (mati lagi saat < 29°C) |
| Kelembaban udara > 82% | Kipas menyala (mati lagi saat < 80%) |
| Jarak sensor ke air ≥ 16,0 cm | **Peringatan "air menipis"** di web (dan notifikasi HP jika diaktifkan) |
| Jarak sensor ke air ≤ 15,0 cm | Peringatan menipis dicabut (hysteresis 1 cm agar tidak berkedip) |
| Jarak sensor ke air ≥ 18,5 cm (tandon kosong) | Pompa diblokir + peringatan "air habis" |
| Jarak sensor ke air ≤ 18,0 cm | Air dianggap tersedia lagi, pompa boleh menyala |

> **Cara membaca HC-SR04:** sensor dipasang di atas dan mengukur jarak ke **permukaan air**. Makin besar angkanya, makin sedikit airnya. 18,5 cm = jarak sensor ke dasar tandon (kosong).

---

## 📲 Notifikasi ke HP (opsional, Telegram)

Peringatan di web hanya terlihat saat halamannya dibuka. Supaya HP berbunyi walau web tertutup, ESP32 bisa mengirim pesan Telegram langsung (tanpa server tambahan):

1. Di Telegram, chat **@BotFather** → `/newbot` → salin *token* bot.
2. Kirim satu pesan apa saja ke bot baru tersebut, lalu buka `https://api.telegram.org/bot<TOKEN>/getUpdates` dan salin angka `"chat":{"id": …}`.
3. Tambahkan dua baris ini ke `secrets.h`:
```cpp
#define TELEGRAM_BOT_TOKEN "123456:ABC..."
#define TELEGRAM_CHAT_ID   "123456789"
```
4. Upload ulang firmware. Jika kedua baris tidak diisi, fitur ini otomatis nonaktif dan firmware berjalan seperti biasa.

Pesan yang dikirim: ⚠️ air menipis (≥ 16 cm), 🚨 air habis (≥ 18,5 cm), dan ✅ air terisi lagi. Pesan tidak dikirim berulang-ulang (jeda minimal 1 menit, pengingat ulang tiap 3 jam selama masih menipis/habis).

---

## 🚀 Cara Menjalankan

### 1. Web App

Web app sudah di-deploy dan dapat diakses di:

```
https://florasync-lite.vercel.app/login.html
```

Untuk menjalankan secara lokal, clone repository ini lalu buka menggunakan Live Server (VS Code extension) karena menggunakan ES Module.

### 2. Firmware ESP32

1. Buka file `florasync_firmware.ino` di Arduino IDE
2. Install library yang diperlukan via Library Manager:
   - `Firebase ESP Client` by Mobizt
   - `DHT sensor library` by Adafruit
   - `Adafruit Unified Sensor` by Adafruit
3. Copy `secrets.h.example` menjadi `secrets.h` (di folder yang sama dengan `.ino`), lalu isi:
```cpp
#define WIFI_SSID       "nama_wifi_kalian"
#define WIFI_PASSWORD   "password_wifi_kalian"
#define DEVICE_EMAIL    "akun_device@email_kalian.com"
#define DEVICE_PASSWORD "password_akun_device"
```
   `DEVICE_EMAIL`/`DEVICE_PASSWORD` adalah akun Firebase Authentication yang dibuat manual lewat Firebase Console (Authentication > Add user), dan UID-nya sudah diizinkan menulis di `firebase-rules.json`. **File `secrets.h` tidak boleh di-commit** — sudah otomatis dikecualikan lewat `.gitignore`.
4. Set board: Tools → Board → ESP32 Dev Module
5. **Kalibrasi cahaya (1 menit):** buka Serial Monitor, catat "LDR ADC" saat sensor ditutup tangan (gelap) dan saat disorot senter (terang). Sesuaikan `LDR_LOW_MAX`, `LDR_HIGH_MIN`, dan `LDR_INVERTED` di firmware (nilai awal 1000 / 3000 / `true` masih perkiraan; `true` karena di modul ini ADC besar = gelap, malam hari terbaca 4095).
6. Upload ke ESP32

> **Firebase Rules:** akun device harus boleh menulis ke `/sensor` (termasuk `lastSeen`), `/actuator`, `/history`, `/diagnostic(s)`, dan `/config`. Sesuaikan `firebase-rules.json` bila belum ada aturan untuk `/config`.

---

## 📁 Struktur File

```
florasync-lite/
├── .gitignore                     # Aturan pengecualian file dari git
├── firebase-config.js             # Konfigurasi Firebase (apiKey publik, aman diekspos)
├── firebase-rules.json            # Security rules Realtime Database (arsip/dokumentasi)
├── secrets.h.example              # Template kredensial WiFi & akun device
├── shared.js                      # Firebase init, auth guard, sidebar, kategori sensor, status perangkat, peringatan air
├── vendor/                        # jsPDF + jsPDF-AutoTable (di-bundle lokal, tidak bergantung CDN)
├── index.html                     # Redirect awal
├── login.html                     # Halaman login
├── dashboard.html                 # Halaman dashboard utama + visualisasi animasi pompa/kipas
├── monitoring.html                # Halaman monitoring real-time + grafik
├── history.html                   # Halaman riwayat data
├── export.html                    # Halaman export CSV & PDF
├── shared.css                     # Style bersama seluruh halaman
└── florasync_firmware.example.ino # Firmware ESP32, versi placeholder tanpa credential
```

> Dua file lain — `florasync_firmware.ino` (firmware asli) dan `secrets.h` (credential asli) — sengaja tidak ada di repo ini. Keduanya dibuat sendiri di komputer masing-masing mengikuti `secrets.h.example` dan panduan di bagian **Firmware ESP32** di atas.



---

## 🛠️ Teknologi yang Digunakan

**Frontend:**
- HTML5, CSS3, Vanilla JavaScript (ES Module)
- Chart.js — visualisasi grafik sensor
- jsPDF + jsPDF-AutoTable — export PDF

**Backend / Database:**
- Firebase Realtime Database — penyimpanan & sinkronisasi real-time
- Firebase Authentication — autentikasi pengguna

**Hardware / Firmware:**
- Arduino IDE (C++)
- Firebase ESP Client Library
- DHT Library (Adafruit)

**Deployment:**
- Vercel — hosting web app
- GitHub — version control

---

## 👥 Tim Pengembang

| Nama | NIM |
|---|---|
| Calvin Andreas | 2702319051 |
| Joshua Jonathan Fariman | 2702233791 |
| Nelson Ferdinand Wangsaputra | 2702228892 |

**Program Studi:** Teknik Informatika  
**School of Computer Science**  
**Universitas Bina Nusantara — Bandung, 2026**

---

## 📄 Lisensi

Proyek ini dikembangkan sebagai karya ilmiah skripsi Universitas Bina Nusantara.  
© 2026 Calvin Andreas, Joshua Jonathan Fariman, Nelson Ferdinand Wangsaputra. All rights reserved.
