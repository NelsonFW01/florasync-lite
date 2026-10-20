// shared.js
// Berisi CSS bersama, sidebar, dan auth guard yang dipakai semua halaman

import { initializeApp } from "https://www.gstatic.com/firebasejs/10.12.0/firebase-app.js";
import { getAuth, signOut, onAuthStateChanged } from "https://www.gstatic.com/firebasejs/10.12.0/firebase-auth.js";
import { getDatabase, ref, onValue, get } from "https://www.gstatic.com/firebasejs/10.12.0/firebase-database.js";
import { firebaseConfig } from "./firebase-config.js";

// Inisialisasi Firebase
export const app = initializeApp(firebaseConfig);
export const auth = getAuth(app);
export const db = getDatabase(app);

// Auth guard: redirect ke login jika belum login
export function requireAuth(callback) {
  onAuthStateChanged(auth, (user) => {
    if (!user) {
      window.location.href = "login.html";
    } else {
      callback(user);
    }
  });
}

// Logout
export async function logout() {
  await signOut(auth);
  window.location.href = "login.html";
}

// ── Firebase push-ID -> timestamp ──────────────────────────────────────────
// Firebase RTDB push keys (from Firebase.RTDB.pushJSON on the ESP32) are NOT
// plain numbers - Number(key) on them is NaN. The first 8 characters encode
// the server's millisecond timestamp in a modified base64 alphabet. This
// decodes that back into a real epoch ms value so /history can be sorted
// and filtered by date.
const PUSH_ID_CHARS =
  "-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";

export function pushIdToTimestamp(pushId) {
  if (!pushId || pushId.length < 8) return NaN;
  let ts = 0;
  for (let i = 0; i < 8; i++) {
    const idx = PUSH_ID_CHARS.indexOf(pushId.charAt(i));
    if (idx === -1) return NaN;
    ts = ts * 64 + idx;
  }
  return ts;
}

// Kebalikan dari pushIdToTimestamp(): dari epoch ms, bikin key batas bawah
// atau batas atas yang valid untuk orderByKey()+startAt()/endAt() di RTDB.
// Karena 8 karakter pertama push-id terurut sama dengan urutan waktu, query
// rentang tanggal bisa dilakukan LANGSUNG oleh server Firebase (cuma
// download data dalam rentang itu), tidak perlu tarik seluruh /history lalu
// difilter di browser - jauh lebih cepat begitu datanya sudah ribuan entri.
export function timestampToPushIdBound(ms, isUpperBound) {
  const chars = [];
  let t = Math.max(0, Math.floor(ms));
  for (let i = 0; i < 8; i++) {
    chars.push(PUSH_ID_CHARS[t % 64]);
    t = Math.floor(t / 64);
  }
  chars.reverse();
  // Bagian acak (12 karakter) diisi ujung alfabet supaya batas bawah pasti
  // <= semua key ber-timestamp sama, dan batas atas pasti >= semuanya.
  const pad = isUpperBound ? PUSH_ID_CHARS[63] : PUSH_ID_CHARS[0];
  return chars.join("") + pad.repeat(12);
}

// ── Kalibrasi & ambang batas (sumber kebenarannya: firmware) ───────────────
// Firmware menulis semua ambang kalibrasinya ke /config setiap kali boot
// (lihat publishConfigToFirebase() di florasync_firmware.ino). Web membaca
// dari sana, jadi angka di layar PASTI sama dengan yang dipakai alat - tidak
// perlu lagi disalin manual dan tidak bisa melenceng seperti sebelumnya.
// Nilai DEFAULT di bawah cuma cadangan kalau /config belum ada (mis. firmware
// belum di-upload ulang) atau gagal dibaca. Isinya = nilai di firmware.
export const DEFAULT_CONFIG = {
  soilDryAdc:      3100,  // ADC > ini -> tanah KERING (pompa boleh menyala)
  soilWetAdc:      3000,  // ADC < ini -> tanah BASAH (pompa berhenti)
  ldrLowMax:       1000,  // kecerahan < ini  -> cahaya RENDAH
  ldrHighMin:      3000,  // kecerahan >= ini -> cahaya TINGGI
  ldrInverted:     true,  // modul LDR ini: ADC BESAR = GELAP (data malam hari terbaca 4095)
  pumpDurationSec: 30,
  pumpCooldownSec: 60,
  fanTempOn:       31,
  fanTempOff:      29,
  fanHumOn:        82,
  fanHumOff:       80,
  // Jarak sensor HC-SR04 ke permukaan air: makin BESAR = air makin sedikit.
  waterWarnCm:      16.0, // jarak >= ini -> peringatan "air menipis" (ke HP/web)
  waterWarnClearCm: 15.0, // jarak <= ini -> peringatan dicabut (= batas toleransi)
  waterEmptyCm:     18.5, // jarak >= ini -> tandon dianggap habis, pompa diblokir
  waterOkCm:        18.0  // jarak <= ini -> pompa boleh menyala lagi setelah habis
};

// Objek "hidup": isinya diperbarui di tempat, jadi cukup baca CONFIG.xxx
// pada saat dipakai (jangan disalin ke variabel di awal halaman).
export const CONFIG = { ...DEFAULT_CONFIG };

// Terima hanya field yang tipenya cocok dengan default - data /config yang
// rusak atau salah ketik tidak akan merusak tampilan.
function applyConfig(raw) {
  if (!raw || typeof raw !== "object") return;
  for (const key of Object.keys(DEFAULT_CONFIG)) {
    const v = raw[key];
    const okType = typeof v === typeof DEFAULT_CONFIG[key];
    if (okType && (typeof v !== "number" || Number.isFinite(v))) CONFIG[key] = v;
  }
}

// Pantau /config terus-menerus (dashboard & monitoring). Dipanggil lagi tiap
// ada perubahan, mis. setelah firmware di-upload ulang dengan kalibrasi baru.
export function watchConfig(onChange) {
  return onValue(
    ref(db, "/config"),
    snap => {
      applyConfig(snap.val());
      if (onChange) onChange(CONFIG);
    },
    err => console.warn("[Config] /config tidak bisa dibaca, pakai nilai bawaan:", err && err.message)
  );
}

// Baca /config sekali (history & export). Tidak pernah melempar error dan
// tidak menahan halaman lebih dari timeoutMs - kalau gagal, nilai bawaan dipakai.
export async function loadConfigOnce(timeoutMs = 3000) {
  try {
    const snap = await Promise.race([
      get(ref(db, "/config")),
      new Promise((_, reject) => setTimeout(() => reject(new Error("timeout")), timeoutMs))
    ]);
    applyConfig(snap.val());
  } catch (e) {
    console.warn("[Config] /config tidak bisa dibaca, pakai nilai bawaan:", e && e.message);
  }
  return CONFIG;
}

// Angka -> teks gaya Indonesia (koma desimal, tanpa pemisah ribuan supaya
// sama dengan angka ADC di kartu sensor: 3100, bukan 3.100).
export function fmtNum(n, decimals) {
  if (n === undefined || n === null || isNaN(n)) return "—";
  const v = decimals === undefined ? Number(n) : Number(n).toFixed(decimals);
  return String(v).replace(".", ",");
}

// ── Kategori yang ramah pengguna (berdasarkan kalibrasi di atas) ───────────
// Angka ADC mentah tetap ditampilkan sebagai info teknis, tapi yang tampil
// utama adalah kata yang gampang dimengerti. tone dipakai untuk warna
// (normal=hijau, warning=kuning, danger=merah, info=biru).
//
// Sensor tanah KAPASITIF: ADC BESAR = KERING. Arah & ambangnya identik dengan
// logika pompa di firmware (ON kalau > soilDryAdc, OFF kalau < soilWetAdc,
// di antaranya = zona hysteresis -> "Lembap").
export function soilCategory(adc, ctx = {}) {
  if (adc === undefined || adc === null || isNaN(adc)) return null;
  const v = Number(adc);
  if (v > CONFIG.soilDryAdc) {
    return ctx.waterEmpty
      ? { key: "dry", label: "Kering", tone: "danger",
          hint: "Tanah kering, tetapi air tandon habis sehingga pompa belum bisa menyiram. Segera isi ulang tandon." }
      : { key: "dry", label: "Kering", tone: "warning",
          hint: "Tanah butuh air. Pompa akan menyiram otomatis." };
  }
  if (v < CONFIG.soilWetAdc) {
    return { key: "wet", label: "Basah", tone: "info",
             hint: "Tanah sudah cukup basah. Pompa tidak perlu menyala." };
  }
  return { key: "moist", label: "Lembap", tone: "normal",
           hint: "Kelembapan tanah sudah pas." };
}

// "Kecerahan" = nilai ADC LDR yang sudah dibuat searah (besar = terang).
// Kalau modul LDR-nya terbalik (ldrInverted), dibalik dulu dengan 4095 - ADC.
export function lightCategory(adc) {
  if (adc === undefined || adc === null || isNaN(adc)) return null;
  const level = CONFIG.ldrInverted ? 4095 - Number(adc) : Number(adc);
  if (level < CONFIG.ldrLowMax) {
    return { key: "low", label: "Rendah", tone: "warning", hint: "Redup atau gelap." };
  }
  if (level >= CONFIG.ldrHighMin) {
    return { key: "high", label: "Tinggi", tone: "info", hint: "Sangat terang." };
  }
  return { key: "ok", label: "Cukup", tone: "normal", hint: "Terang yang cukup." };
}

// Rentang ADC tiap kategori, untuk legenda ("Arti Kondisi Sensor") di
// Monitoring dan catatan kaki di History/Export. Ditulis dalam ADC mentah.
export function conditionRanges() {
  const c = CONFIG;
  const lowRaw  = c.ldrInverted ? 4095 - c.ldrLowMax  : c.ldrLowMax;
  const highRaw = c.ldrInverted ? 4095 - c.ldrHighMin : c.ldrHighMin;
  return {
    soil: [
      { label: "Kering", tone: "warning", range: "lebih dari " + c.soilDryAdc + " ADC" },
      { label: "Lembap", tone: "normal",  range: c.soilWetAdc + " – " + c.soilDryAdc + " ADC" },
      { label: "Basah",  tone: "info",    range: "kurang dari " + c.soilWetAdc + " ADC" }
    ],
    light: c.ldrInverted ? [
      { label: "Rendah", tone: "warning", range: "lebih dari " + lowRaw + " ADC" },
      { label: "Cukup",  tone: "normal",  range: (highRaw + 1) + " – " + lowRaw + " ADC" },
      { label: "Tinggi", tone: "info",    range: highRaw + " ADC atau kurang" }
    ] : [
      { label: "Rendah", tone: "warning", range: "kurang dari " + lowRaw + " ADC" },
      { label: "Cukup",  tone: "normal",  range: lowRaw + " – " + (highRaw - 1) + " ADC" },
      { label: "Tinggi", tone: "info",    range: highRaw + " ADC atau lebih" }
    ]
  };
}

// Kalimat satu baris untuk catatan kaki tabel History/Export.
export function conditionNote() {
  const r = conditionRanges();
  const fmt = list => list.map(x => x.label + " (" + x.range + ")").join(", ");
  return "Kondisi dihitung dari nilai sensor (ADC) berdasarkan kalibrasi alat. " +
         "Tanah: " + fmt(r.soil) + ". Cahaya: " + fmt(r.light) + ".";
}

// Badge HTML untuk satu kategori (isi dari konstanta di atas, aman dari XSS).
export function catBadgeHtml(cat) {
  return cat ? '<span class="badge badge-' + cat.tone + '">' + cat.label + "</span>" : "—";
}

// Klasifikasi status air, 3 tingkat:
//   normal  (Cukup)   : jarak <= waterWarnClearCm (15 cm)
//   warning (Menipis) : jarak >= waterWarnCm (16 cm)  -> peringatan, pompa MASIH boleh jalan
//   danger  (Habis)   : jarak >= waterEmptyCm (18,5 cm) -> pompa diblokir
// Flag /sensor/waterEmpty dan /sensor/waterLow dihitung di firmware (pakai
// hysteresis + fail-safe sensor gagal), jadi itu sumber kebenarannya. Kalau
// flag waterLow tidak ada (firmware lama), dipakai jarak mentahnya sebagai cadangan.
// Sensor mengukur JARAK sensor->permukaan air, jadi angka BESAR = air SEDIKIT.
export function waterState(distanceCm, isEmptyFlag, isLowFlag) {
  if (isEmptyFlag === true) return "danger";
  if (typeof isLowFlag === "boolean") return isLowFlag ? "warning" : "normal";
  if (distanceCm === undefined || distanceCm === null) return "normal";
  if (distanceCm >= CONFIG.waterEmptyCm) return "danger";
  if (distanceCm >= CONFIG.waterWarnCm) return "warning";
  return "normal";
}
export const WATER_LABEL = { normal: "Cukup", warning: "Menipis", danger: "Habis" };

// Peringatan air tandon untuk Dashboard & Monitoring (isi dan kata-katanya
// sama di dua halaman). Mengembalikan null kalau tidak perlu peringatan.
// Saat perangkat terputus tidak ditampilkan: itu data lama, dan penjelasannya
// sudah ada di banner status perangkat.
export function waterAlertInfo(d, offline) {
  if (!d || offline) return null;
  const dist  = d.waterLevel;
  const where = "Jarak sensor ke air " + (dist !== undefined && dist !== null ? fmtNum(dist, 1) : "?") + " cm";
  const state = waterState(dist, d.waterEmpty, d.waterLow);
  if (state === "danger") {
    return { level: "danger", icon: "🚨", title: "Air tandon habis.",
      text: " " + where + " (batas kosong " + fmtNum(CONFIG.waterEmptyCm, 1) +
            " cm). Pompa dimatikan dan tidak akan menyala sampai tandon diisi ulang." };
  }
  if (state === "warning") {
    return { level: "warning", icon: "⚠️", title: "Air tandon menipis.",
      text: " " + where + " (batas aman " + fmtNum(CONFIG.waterWarnClearCm, 1) +
            " cm). Segera isi ulang sebelum air habis." };
  }
  return null;
}

export function applyWaterAlert(el, d, offline) {
  if (!el) return;
  const info = waterAlertInfo(d, offline);
  el.className = "alert-banner" + (info ? " show " + info.level : "");
  el.replaceChildren();
  if (!info) return;
  const icon = document.createElement("span");
  icon.textContent = info.icon;
  const body = document.createElement("span");
  const strong = document.createElement("strong");
  strong.textContent = info.title;
  body.append(strong, document.createTextNode(info.text));
  el.append(icon, body);
}

// ── Status perangkat (dipakai Dashboard DAN Monitoring) ────────────────────
// Satu-satunya tempat yang memutuskan "perangkat terhubung / terputus /
// sedang diperiksa", supaya kedua halaman SELALU menampilkan hal yang sama
// dengan kata-kata yang sama. Tiga status saja:
//   checking : belum ada bukti apa pun (maksimal DEVICE_CHECK_TIMEOUT_MS)
//   online   : ada data baru dari alat dalam DEVICE_STALE_MS terakhir
//   offline  : tidak ada data baru (atau data yang ada sudah basi)
//
// Cara menilai "data baru":
//  - Firmware baru menulis /sensor/lastSeen (jam SERVER Firebase) tiap siklus.
//    Dibandingkan dengan jam server (.info/serverTimeOffset), jadi status
//    langsung pasti begitu halaman dibuka dan tidak terpengaruh jam komputer
//    pengguna yang meleset. Nilainya SELALU berubah, jadi tidak ada lagi
//    kasus "alat hidup tapi dianggap mati karena angka sensor kebetulan sama".
//  - Firmware lama (tanpa lastSeen): cara lama dipakai sebagai cadangan -
//    snapshot pertama = data tersimpan (belum bukti), snapshot kedua = bukti.
export const DEVICE_CHECK_TIMEOUT_MS = 12000;
export const DEVICE_STALE_MS         = 20000;

const DEVICE_BADGE = {
  checking: "Memeriksa koneksi…",
  online:   "Perangkat terhubung",
  offline:  "Perangkat terputus"
};

// Teks status aktuator. "Tidak diketahui" dipakai (bukan "Offline"/"Mati")
// karena saat perangkat terputus kita memang TIDAK tahu pompa/kipas sedang
// menyala atau tidak - nilai ON/OFF terakhir di database sudah basi.
export function actuatorView(isOn, deviceState) {
  if (deviceState === "online" && isOn !== null && isOn !== undefined) {
    return isOn ? { text: "Menyala", cls: "tag-on", on: true }
                : { text: "Mati",    cls: "tag-off", on: false };
  }
  if (deviceState === "offline") return { text: "Tidak diketahui", cls: "tag-unknown", on: false };
  return { text: "Menunggu data…", cls: "tag-unknown", on: false };
}

// Jam singkat: hanya jam kalau hari ini, tanggal + jam kalau hari lain.
export function fmtClock(ms) {
  if (!ms) return "—";
  const d = new Date(ms);
  return d.toDateString() === new Date().toDateString()
    ? d.toLocaleTimeString("id-ID")
    : d.toLocaleDateString("id-ID") + " " + d.toLocaleTimeString("id-ID");
}

export function deviceMessage(state, lastSeenMs, lastAct) {
  if (state === "checking") {
    return { icon: "⏳", text: "Sedang memeriksa koneksi perangkat (maksimal " +
      (DEVICE_CHECK_TIMEOUT_MS / 1000) + " detik). Status pompa dan kipas akan tampil setelah koneksi terkonfirmasi." };
  }
  if (state === "offline") {
    return { icon: "⚠️", text: "Perangkat tidak terhubung, sehingga status pompa dan kipas saat ini tidak diketahui. " +
      (lastSeenMs ? "Data terakhir tercatat " + fmtClock(lastSeenMs) + ". " : "") +
      (lastAct ? "Status terakhir yang tercatat: pompa " + (lastAct.pump ? "menyala" : "mati") +
                 ", kipas " + (lastAct.fan ? "menyala" : "mati") + " (belum tentu sama dengan sekarang). " : "") +
      "Angka sensor di bawah adalah nilai terakhir yang tersimpan, bukan kondisi saat ini." };
  }
  return null; // online: tidak perlu pesan
}

// Pasang tampilan status ke elemen halaman: { badge, banner, main }.
export function applyDeviceUi(els, state, lastSeenMs, lastAct) {
  if (els.badge) {
    els.badge.className = "badge badge-" + (state === "online" ? "online" : state === "checking" ? "checking" : "offline");
    els.badge.innerHTML = '<span class="dot"></span> ' + DEVICE_BADGE[state];
  }
  if (els.banner) {
    const msg = deviceMessage(state, lastSeenMs, lastAct);
    els.banner.className = "device-banner" + (msg ? " show " + state : "");
    els.banner.replaceChildren();
    if (msg) {
      const icon = document.createElement("span");
      icon.textContent = msg.icon;
      const text = document.createElement("span");
      text.textContent = msg.text;
      els.banner.append(icon, text);
    }
  }
  // Saat terputus, kartu sensor diredupkan supaya jelas itu data lama.
  if (els.main) els.main.classList.toggle("device-offline", state === "offline");
}

// Buat pemantau status. onChange(state, lastSeenMs) dipanggil sekali di awal
// (state "checking") lalu setiap status berubah. Panggil monitor.feed(d)
// untuk SETIAP snapshot /sensor yang masuk.
export function createDeviceMonitor(onChange, opts = {}) {
  const staleMs  = opts.staleMs  ?? DEVICE_STALE_MS;
  const giveUpMs = opts.giveUpMs ?? DEVICE_CHECK_TIMEOUT_MS;

  let state        = "checking";
  let offset       = null;  // jam server - jam browser (ms); null = belum diketahui
  let heartbeat    = null;  // nilai /sensor/lastSeen terakhir (epoch ms, jam server)
  let legacy       = false; // true = firmware lama tanpa lastSeen
  let rxCount      = 0;     // jumlah snapshot /sensor yang sudah masuk
  let lastLiveLocal = 0;    // mode lama: kapan snapshot "baru" terakhir diterima

  function lastSeenMs() {
    return heartbeat !== null ? heartbeat : (lastLiveLocal || null);
  }

  function setState(next) {
    if (next === state) return;
    state = next;
    onChange(state, lastSeenMs());
  }

  function evaluate() {
    const now = Date.now();
    if (!legacy && heartbeat !== null) {
      if (offset === null) return; // tunggu selisih jam server dulu (cuma sesaat)
      setState(now + offset - heartbeat <= staleMs ? "online" : "offline");
    } else if (legacy) {
      if (!lastLiveLocal) return;  // masih menunggu snapshot kedua
      setState(now - lastLiveLocal <= staleMs ? "online" : "offline");
    }
  }

  function feed(d) {
    rxCount++;
    const hb = d ? Number(d.lastSeen) : NaN;
    if (Number.isFinite(hb) && hb > 0) {
      legacy    = false;
      heartbeat = hb;
    } else {
      legacy = true;
      if (rxCount >= 2) lastLiveLocal = Date.now();
    }
    evaluate();
  }

  onValue(ref(db, ".info/serverTimeOffset"), snap => {
    offset = Number(snap.val()) || 0;
    evaluate();
  });

  // Dievaluasi ulang tiap 3 detik supaya data yang berhenti masuk terdeteksi.
  setInterval(evaluate, 3000);

  // Pengaman: kalau sampai batas ini belum ada bukti apa pun (database
  // kosong / alat mati), jangan menggantung di "memeriksa" - putuskan offline.
  // Timer ini berdiri sendiri, tidak bergantung pada pemuatan data lain.
  setTimeout(() => { if (state === "checking") setState("offline"); }, giveUpMs);

  onChange(state, null);

  return {
    feed,
    get state()      { return state; },
    get lastSeenMs() { return lastSeenMs(); }
  };
}

// Build sidebar HTML
export function buildSidebar(activePage) {
  const navItems = [
    {
      id: "dashboard",
      label: "Dashboard",
      href: "dashboard.html",
      icon: `<path stroke-linecap="round" stroke-linejoin="round" d="M3 12l2-2m0 0l7-7 7 7M5 10v10a1 1 0 001 1h3m10-11l2 2m-2-2v10a1 1 0 01-1 1h-3m-6 0a1 1 0 001-1v-4a1 1 0 011-1h2a1 1 0 011 1v4a1 1 0 001 1m-6 0h6"/>`
    },
    {
      id: "monitoring",
      label: "Monitoring",
      href: "monitoring.html",
      icon: `<path stroke-linecap="round" stroke-linejoin="round" d="M9 19v-6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2a2 2 0 002-2zm0 0V9a2 2 0 012-2h2a2 2 0 012 2v10m-6 0a2 2 0 002 2h2a2 2 0 002-2m0 0V5a2 2 0 012-2h2a2 2 0 012 2v14a2 2 0 01-2 2h-2a2 2 0 01-2-2z"/>`
    },
    {
      id: "history",
      label: "Data History",
      href: "history.html",
      icon: `<path stroke-linecap="round" stroke-linejoin="round" d="M12 8v4l3 3m6-3a9 9 0 11-18 0 9 9 0 0118 0z"/>`
    },
    {
      id: "export",
      label: "Export Data",
      href: "export.html",
      icon: `<path stroke-linecap="round" stroke-linejoin="round" d="M4 16v1a3 3 0 003 3h10a3 3 0 003-3v-1m-4-4l-4 4m0 0l-4-4m4 4V4"/>`
    }
  ];

  return `
    <aside class="sidebar">
      <div class="sidebar-logo">
        <div class="logo-icon">
          <svg viewBox="0 0 24 24" fill="white" width="20" height="20">
            <path d="M17 8C8 10 5.9 16.17 3.82 21.34L5.71 22l1-2.3A4.49 4.49 0 008 20C19 20 22 3 22 3c-1 2-8 2-8 2 3-3 8-2 8-2C19 14 12 19 7 19a6.65 6.65 0 01-3.41-.93L3 21"/>
          </svg>
        </div>
        <div>
          <div class="logo-name">FloraSync Lite</div>
          <div class="logo-sub">IoT Greenhouse</div>
        </div>
      </div>

      <nav class="sidebar-nav">
        ${navItems.map(item => `
          <a href="${item.href}" class="nav-item ${activePage === item.id ? 'active' : ''}">
            <svg class="nav-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">
              ${item.icon}
            </svg>
            <span>${item.label}</span>
          </a>
        `).join('')}
      </nav>

      <button class="logout-btn" id="logoutBtn">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" width="16" height="16">
          <path stroke-linecap="round" stroke-linejoin="round" d="M17 16l4-4m0 0l-4-4m4 4H7m6 4v1a3 3 0 01-3 3H6a3 3 0 01-3-3V7a3 3 0 013-3h4a3 3 0 013 3v1"/>
        </svg>
        <span>Logout</span>
      </button>

      <div class="sidebar-footer">© 2026 Binus University</div>
    </aside>
  `;
}

// (sharedCSS dihapus - tidak dipakai di manapun; styling asli ada di shared.css)
