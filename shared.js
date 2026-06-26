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

// CSS bersama yang dipakai semua halaman
export const sharedCSS = `
  *, *::before, *::after {
    box-sizing: border-box;
    margin: 0;
    padding: 0;
  }

  :root {
    --green-50:  #EAF3DE;
    --green-100: #C0DD97;
    --green-200: #97C459;
    --green-400: #639922;
    --green-600: #3B6D11;
    --green-800: #27500A;
    --teal-50:   #E1F5EE;
    --teal-400:  #1D9E75;
    --teal-600:  #0F6E56;
    --gray-50:   #F4F3EF;
    --gray-100:  #D3D1C7;
    --gray-400:  #888780;
    --gray-600:  #5F5E5A;
    --amber-50:  #FAEEDA;
    --amber-400: #BA7517;
    --red-50:    #FCEBEB;
    --red-400:   #E24B4A;
    --red-600:   #A32D2D;
    --blue-50:   #E6F1FB;
    --blue-400:  #378ADD;
    --blue-600:  #185FA5;
  }

  body {
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
    background: var(--gray-50);
    display: flex;
    min-height: 100vh;
    color: #1a1a1a;
  }

  /* ===== SIDEBAR ===== */
  .sidebar {
    width: 220px;
    min-height: 100vh;
    background: var(--green-800);
    display: flex;
    flex-direction: column;
    padding: 1.25rem 0;
    flex-shrink: 0;
    position: sticky;
    top: 0;
    height: 100vh;
    overflow-y: auto;
  }

  .sidebar-logo {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 0 1.25rem 1.25rem;
    border-bottom: 1px solid rgba(255,255,255,0.1);
    margin-bottom: 0.5rem;
  }

  .logo-icon {
    width: 36px;
    height: 36px;
    background: var(--green-400);
    border-radius: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .logo-name {
    font-size: 14px;
    font-weight: 600;
    color: #fff;
    line-height: 1.2;
  }

  .logo-sub {
    font-size: 10px;
    color: rgba(255,255,255,0.5);
  }

  .sidebar-nav {
    flex: 1;
    padding: 0.5rem 0.75rem;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .nav-item {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 9px 12px;
    border-radius: 8px;
    text-decoration: none;
    color: rgba(255,255,255,0.6);
    font-size: 13.5px;
    transition: background 0.15s, color 0.15s;
  }

  .nav-item:hover {
    background: rgba(255,255,255,0.1);
    color: #fff;
  }

  .nav-item.active {
    background: var(--green-600);
    color: #fff;
  }

  .nav-icon {
    width: 18px;
    height: 18px;
    flex-shrink: 0;
  }

  .logout-btn {
    margin: 0.5rem 0.75rem 0;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 9px 12px;
    border-radius: 8px;
    border: none;
    background: rgba(255,255,255,0.07);
    color: rgba(255,255,255,0.6);
    font-size: 13px;
    cursor: pointer;
    width: calc(100% - 1.5rem);
    transition: background 0.15s, color 0.15s;
  }

  .logout-btn:hover {
    background: rgba(220,50,50,0.2);
    color: #ff8a80;
  }

  .sidebar-footer {
    margin-top: 1rem;
    text-align: center;
    font-size: 10px;
    color: rgba(255,255,255,0.25);
    padding: 0 1rem;
  }

  /* ===== MAIN CONTENT ===== */
  .main {
    flex: 1;
    padding: 2rem;
    overflow-y: auto;
    min-width: 0;
  }

  .page-header {
    margin-bottom: 1.5rem;
  }

  .page-title {
    font-size: 22px;
    font-weight: 600;
    color: var(--green-800);
    letter-spacing: -0.3px;
  }

  .page-subtitle {
    font-size: 13px;
    color: var(--gray-400);
    margin-top: 3px;
  }

  /* ===== CARDS ===== */
  .card {
    background: #fff;
    border-radius: 12px;
    border: 1px solid var(--gray-100);
    padding: 1.25rem;
    margin-bottom: 1.25rem;
  }

  .card-title {
    font-size: 11px;
    font-weight: 600;
    color: var(--gray-400);
    text-transform: uppercase;
    letter-spacing: 0.6px;
    margin-bottom: 1rem;
  }

  /* ===== GRID ===== */
  .grid-5 {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
    gap: 12px;
    margin-bottom: 1.25rem;
  }

  .grid-2 {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
    gap: 12px;
    margin-bottom: 1.25rem;
  }

  /* ===== STAT CARD ===== */
  .stat-card {
    background: #fff;
    border-radius: 12px;
    border: 1px solid var(--gray-100);
    padding: 1rem 1.1rem;
  }

  .stat-label {
    font-size: 12px;
    color: var(--gray-400);
    margin-bottom: 6px;
    display: flex;
    align-items: center;
    gap: 5px;
  }

  .stat-value {
    font-size: 28px;
    font-weight: 700;
    color: #1a1a1a;
    line-height: 1;
    margin-bottom: 8px;
  }

  .stat-unit {
    font-size: 14px;
    font-weight: 400;
    color: var(--gray-400);
    margin-left: 1px;
  }

  /* ===== BADGES ===== */
  .badge {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 3px 10px;
    border-radius: 20px;
    font-size: 11px;
    font-weight: 600;
  }

  .badge-normal  { background: var(--green-50);  color: var(--green-600); }
  .badge-warning { background: var(--amber-50);  color: var(--amber-400); }
  .badge-danger  { background: var(--red-50);    color: var(--red-600);   }
  .badge-online  { background: var(--teal-50);   color: var(--teal-600);  }
  .badge-offline { background: var(--gray-50);   color: var(--gray-400);  }

  .dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: currentColor;
    display: inline-block;
  }

  /* ===== ACTUATOR ===== */
  .actuator-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 10px 0;
    border-bottom: 1px solid var(--gray-100);
  }

  .actuator-row:last-child { border-bottom: none; }

  .actuator-name {
    font-size: 14px;
    font-weight: 500;
    display: flex;
    align-items: center;
    gap: 8px;
    color: #333;
  }

  .act-icon {
    width: 30px;
    height: 30px;
    border-radius: 7px;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .act-on  { background: var(--teal-50); }
  .act-off { background: var(--gray-50); }

  .tag-on  { background: var(--teal-50);  color: var(--teal-600);  padding: 4px 12px; border-radius: 20px; font-size: 12px; font-weight: 700; }
  .tag-off { background: var(--gray-50);  color: var(--gray-400);  padding: 4px 12px; border-radius: 20px; font-size: 12px; font-weight: 600; }

  /* ===== BUTTONS ===== */
  .btn-primary {
    padding: 9px 20px;
    background: var(--green-400);
    color: #fff;
    border: none;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.2s;
  }

  .btn-primary:hover { background: var(--green-600); }
  .btn-primary:disabled { background: var(--gray-100); color: var(--gray-400); cursor: not-allowed; }

  .btn-secondary {
    padding: 9px 20px;
    background: var(--gray-100);
    color: var(--gray-600);
    border: none;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.2s;
  }

  .btn-secondary:hover { background: var(--gray-400); color: #fff; }

  /* ===== TABLE ===== */
  .table-wrap {
    overflow-x: auto;
    border-radius: 10px;
    border: 1px solid var(--gray-100);
  }

  table {
    width: 100%;
    border-collapse: collapse;
    background: #fff;
    font-size: 13px;
  }

  thead th {
    text-align: left;
    padding: 10px 14px;
    background: var(--gray-50);
    color: var(--gray-600);
    font-weight: 600;
    font-size: 11px;
    border-bottom: 1px solid var(--gray-100);
    white-space: nowrap;
    text-transform: uppercase;
    letter-spacing: 0.4px;
  }

  tbody td {
    padding: 10px 14px;
    border-bottom: 1px solid var(--gray-100);
    color: #1a1a1a;
    white-space: nowrap;
  }

  tbody tr:last-child td { border-bottom: none; }
  tbody tr:hover { background: var(--green-50); }

  .empty-state {
    text-align: center;
    padding: 3rem;
    color: var(--gray-400);
    font-size: 14px;
  }

  /* ===== FORM ===== */
  .filter-row {
    display: flex;
    align-items: flex-end;
    gap: 12px;
    flex-wrap: wrap;
    margin-bottom: 1.25rem;
  }

  .filter-group {
    display: flex;
    flex-direction: column;
    gap: 5px;
  }

  .filter-group label {
    font-size: 12px;
    color: var(--gray-400);
    font-weight: 500;
  }

  .filter-group input[type="date"] {
    padding: 8px 10px;
    border: 1px solid var(--gray-100);
    border-radius: 8px;
    font-size: 13px;
    color: #1a1a1a;
    outline: none;
    font-family: inherit;
    background: #fff;
  }

  .filter-group input[type="date"]:focus {
    border-color: var(--green-400);
    box-shadow: 0 0 0 3px rgba(99,153,34,0.1);
  }

  /* ===== RESPONSIVE ===== */
  @media (max-width: 640px) {
    .sidebar {
      width: 56px;
    }
    .logo-name, .logo-sub, .nav-item span,
    .logout-btn span, .sidebar-footer {
      display: none;
    }
    .main { padding: 1rem; }
  }
`;
