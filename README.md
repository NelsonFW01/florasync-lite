# 🌱 FloraSync Lite

> **Greenhouse Portable IoT Skala Mikro Hemat Energi untuk Budidaya Tanaman Hortikultura**
>
> Skripsi — Program Studi Teknik Informatika, Universitas Bina Nusantara, 2026

---

## 📖 Tentang Proyek

FloraSync Lite adalah sistem greenhouse portabel skala mikro berbasis Internet of Things (IoT) yang dirancang untuk mendukung kegiatan urban farming di lingkungan perkotaan. Sistem ini memungkinkan pemantauan dan pengendalian kondisi lingkungan tanaman secara real-time melalui aplikasi web, dengan pendekatan desain daya rendah (*low-power design*) menggunakan adaptor 5V/2A.

**Dimensi greenhouse:** 55 × 45 × 40 cm  
**Tanaman:** Pakcoy, Cabai Rawit, Kemangi  
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
| **Dashboard** | Ringkasan kondisi sensor & status aktuator real-time |
| **Monitoring** | Nilai sensor detail + grafik tren 20 pembacaan terakhir |
| **Data History** | Riwayat data dengan filter rentang tanggal |
| **Export Data** | Download data dalam format CSV atau PDF |

---

## ⚙️ Logika Kontrol Otomatis ESP32

| Kondisi | Aksi |
|---|---|
| Kelembaban tanah < 2200 ADC | Pompa ON selama 10 detik |
| Suhu udara > 30°C | Kipas ON |
| Kelembaban udara > 80% | Kipas ON |
| Pasca penyiraman (90 detik) | Kipas ON |
| Cooldown antar siram | 60 detik |
| Level air < 5 cm | Alert di dashboard |

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
3. Ganti konfigurasi WiFi di bagian atas file:
```cpp
#define WIFI_SSID      "nama_wifi_kalian"
#define WIFI_PASSWORD  "password_wifi_kalian"
```
4. Set board: Tools → Board → ESP32 Dev Module
5. Upload ke ESP32

---

## 📁 Struktur File

```
florasync-lite/
├── firebase-config.js      # Konfigurasi Firebase
├── shared.js               # CSS bersama, sidebar, auth guard
├── login.html              # Halaman login
├── dashboard.html          # Halaman dashboard utama
├── monitoring.html         # Halaman monitoring real-time + grafik
├── history.html            # Halaman riwayat data
├── export.html             # Halaman export CSV & PDF
└── florasync_firmware.ino  # Firmware ESP32
```

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
