// shared.js
// Berisi CSS bersama, sidebar, dan auth guard yang dipakai semua halaman

import { initializeApp } from "https://www.gstatic.com/firebasejs/10.12.0/firebase-app.js";
import { getAuth, signOut, onAuthStateChanged } from "https://www.gstatic.com/firebasejs/10.12.0/firebase-auth.js";
import { getDatabase } from "https://www.gstatic.com/firebasejs/10.12.0/firebase-database.js";
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

// ── Water level thresholds (harus sama dengan firmware .ino) ──────────────
// WATER_LEVEL_EMPTY_CM / WATER_LEVEL_OK_CM di florasync_firmware.ino.
// Sensor mengukur JARAK sensor->permukaan air, jadi angka BESAR = air
// SEDIKIT. Kalau nilai ini diubah di firmware, ubah juga di sini.
export const WATER_LEVEL_EMPTY_CM = 18.5;
export const WATER_LEVEL_OK_CM    = 12.5;

// Klasifikasi status air. Mengutamakan flag /sensor/waterEmpty (dihitung di
// firmware dengan hysteresis + fail-safe sensor gagal) sebagai sumber
// kebenaran untuk status "danger". Raw distance dipakai untuk membedakan
// "warning" (di zona hysteresis, belum genap kosong) dari "normal".
export function waterState(distanceCm, isEmptyFlag) {
  if (isEmptyFlag === true) return "danger";
  if (distanceCm === undefined || distanceCm === null) return "normal";
  if (distanceCm >= WATER_LEVEL_EMPTY_CM) return "danger";
  if (distanceCm > WATER_LEVEL_OK_CM) return "warning";
  return "normal";
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
        Logout
      </button>

      <div class="sidebar-footer">© 2026 Binus University</div>
    </aside>
  `;
}

// (sharedCSS dihapus - tidak dipakai di manapun; styling asli ada di shared.css)
