const ESP32_IP = 'http://192.168.1.100',
  LONG_MS = 1000,
  SHORT_MS = 200,
  state = {
  lights: [!1, !1],
  poolOn: !1,
  valves: [
    {isOpen: !1, autoEnabled: !1,
      slots: [
        {active: !1, h: 7, m: 0, dur: 10},
        {active: !1, h: 10, m: 0, dur: 10},
        {active: !1, h: 13, m: 0, dur: 10},
        {active: !1, h: 16, m: 0, dur: 10}
      ],
      manualTimeout: 10
    },
    {isOpen: !1, autoEnabled: !1,
      slots: [
        {active: !1 ,h: 7, m: 11, dur: 10},
        {active: !1 ,h: 10, m: 11, dur: 10},
        {active: !1 ,h: 13, m: 11, dur: 10},
        {active: !1 ,h: 16, m: 11, dur: 10}
      ],
      manualTimeout: 10
    }
  ],
  feeder: {
    isSleeping: false,
    batPct: null,
    foodPct: null,
    autoEnabled: !1,
    slots: [
      {active: !1, h: 6, m: 30}, {active: !1, h: 12, m: 30},
      {active: !1, h: 18, m: 30}, {active: !1, h: 0, m: 30}
    ],
    sleep: { active: !1, startH: 22, startM: 0, endH: 6, endM: 0 }
  }
};

// Web sürümü geri tuşu: uygulamada geri tuşunu MainActivity yönetir (handleBack); tarayıcıda sayfa
// derinliğine göre geçmiş (history) kaydı tutulur. Ana panelde geri = çıkış. Uygulamada (Android köprüsü varsa) hiçbir şey yapmaz.
const PAGE_DEPTH = { dash: 0, lights: 1, water: 1, feeder: 1, config: 1, 'vcfg-0': 2, 'vcfg-1': 2, 'feeder-config': 2 };
let histDepth = 0, histIgnore = false, histFromPop = false;

function histSync(page) {
  if (window.Android || histFromPop) return;
  const d = PAGE_DEPTH[page] || 0;
  try {
    if (d > histDepth) { for (let i = histDepth + 1; i <= d; i++) history.pushState({ d: i }, ''); }
    else if (d < histDepth) { histIgnore = true; setTimeout(() => { histIgnore = false; }, 500); history.go(d - histDepth); }
    else history.replaceState({ d }, '');
    histDepth = d;
  } catch (e) {}
}

if (!window.Android) {
  try { history.replaceState({ d: 0 }, ''); } catch (e) {}
  window.addEventListener('popstate', ev => {
    if (histIgnore) { histIgnore = false; return; }
    const cur = document.querySelector('.page.active');
    let id = cur ? cur.id.replace('page-', '') : 'dash';
    const curD = PAGE_DEPTH[id] || 0;
    const newD = (ev.state && ev.state.d) || 0;
    histDepth = newD;
    if (newD < curD) {
      while ((PAGE_DEPTH[id] || 0) > newD) id = PAGE_PARENT[id] || 'dash';
      histFromPop = true; showPage(id); histFromPop = false;
    } else if (newD > curD) {
      histDepth = curD;
      try { history.replaceState({ d: curD }, ''); } catch (e) {}
    }
  });
}

function showPage(e) {
  const rd = document.getElementById('restart-dlg'); if (rd) rd.hidden = true;
  document.querySelectorAll('.page').forEach(t => t.classList.remove('active'));
  document.getElementById('page-' + e).classList.add('active');
  const t = {
    dash: '🏠 Ana Panel',
    lights: '💡 Aydınlatma',
    water: '💧 Sulama',
    'vcfg-0': '💧 Sulama Vanası 1',
    'vcfg-1': '💧 Sulama Vanası 2',
    feeder: '🐱 Mama Kabı',
    'feeder-config': '🐱 Otomatik Yemleme',
    config: '⚙ Bağlantı Ayarları'
  };
  document.getElementById('header-title').textContent = t[e] || '🏠 Ana Panel';
  e === 'vcfg-0' && fetchValveCfg(0),
  e === 'vcfg-1' && fetchValveCfg(1),
  e === 'feeder-config' && renderFeedConfig();
  if (e === 'feeder') {
    refreshFeederCache();
    fetchFeederStatus();
    fetchFeederConfig();
  }
  histSync(e);
}

// Android geri tuşu: alt panelde bir üst panele dön, ana panelde false döndür (uygulama kapanır)
const PAGE_PARENT = {
  lights: 'dash',
  water: 'dash',
  feeder: 'dash',
  'vcfg-0': 'water',
  'vcfg-1': 'water',
  'feeder-config': 'feeder',
  config: 'dash'
};

function handleBack() {
  const cur = document.querySelector('.page.active');
  const id = cur ? cur.id.replace('page-', '') : 'dash';
  const parent = PAGE_PARENT[id];
  if (!parent) return false;
  showPage(parent);
  return true;
}
window.handleBack = handleBack;

// ── CİHAZ SAYFASI (SALT OKUNUR): ESP32 tanısı + günlük (WS/MQTT, yalnız GET) ──
let devLogStart = -1;   // ekrandaki log metninin başladığı bayt konumu (0 = dosya başına ulaşıldı)
let devLogText = '';
let devBusy = false;

function devFmtDur(s) {
  if (s < 0) return '-';
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + ' g ' : '') + (h || d ? h + ' sa ' : '') + m + ' dk';
}

function devRenderStatus(d) {
  const kb = n => (n / 1024).toFixed(1) + ' KB';
  const L = [];
  L.push('Çalışma süresi: ' + devFmtDur(d.uptimeSec));
  L.push('Boş bellek: ' + kb(d.freeHeap) + ' (en düşük ' + kb(d.minFreeHeap) + ')');
  L.push('Yazılım: ' + d.fw);
  L.push('Son reset: ' + d.resetReason);
  L.push('NTP: ' + (d.ntpAgeSec >= 0 ? devFmtDur(d.ntpAgeSec) + ' önce eşitlendi' : 'eşitlenmedi'));
  const w = d.wifi || {};
  L.push('WiFi: ' + (w.connected ? 'bağlı, ' + w.rssi + ' dBm, kanal ' + w.channel + ', ' + w.ip : 'bağlı değil'));
  const m = d.mqtt || {};
  L.push('MQTT: ' + (!m.enabled ? 'kapalı (ayar yok)' : m.up ? 'bağlı' : 'bağlı değil (hata ' + m.fails + ')'));
  L.push('Yerel WS istemci: ' + d.wsClients);
  const f = d.fs || {};
  L.push('Dosya sistemi: ' + kb(f.used) + ' / ' + kb(f.total) + ', log ' + kb(f.log));
  const p = d.feederPoll || {};
  L.push('Feeder sorgu: pencere ' + (p.window ? 'açık' : 'kapalı') + ', kanal ' + p.channel + ', ardışık hata ' + p.fails + (p.retry ? ', yeniden deneme bekliyor' : ''));
  if (p.window && p.nextPeriodicSec >= 0) L.push('Sonraki saatlik sorgu: ~' + Math.max(0, Math.round(p.nextPeriodicSec / 60)) + ' dk sonra');
  document.getElementById('dev-status').textContent = L.join('\n');
}

function devRenderLog(scrollEnd) {
  const el = document.getElementById('dev-log');
  el.textContent = devLogText.trim() ? devLogText : 'Henüz log kaydı yok.';
  if (scrollEnd) el.scrollTop = el.scrollHeight;
  const b = document.getElementById('dev-log-older');
  if (b) b.disabled = devLogStart === 0;
}

async function devRefresh() {
  if (devBusy) return; if (!requireConnection(true)) { const s = document.getElementById('dev-status'); if (s) s.textContent = 'Bağlantı yok. Bağlandıktan sonra Yenile’ye basın.'; return; }
  devBusy = true;
  try {
    const diag = await (await apiFetch(ESP32_IP + '/api/diag')).json();
    devRenderStatus(diag);
    const c = await (await apiFetch(ESP32_IP + '/api/log-chunk')).json();
    devLogText = c.text || ''; devLogStart = c.start || 0;
    devRenderLog(true);
  } catch (err) {
    showToast(cmdError(err), true);
  } finally { devBusy = false; }
}

async function devLogOlder() {
  if (devBusy || devLogStart <= 0 || !requireConnection()) return;
  devBusy = true;
  try {
    const c = await (await apiFetch(ESP32_IP + '/api/log-chunk?end=' + devLogStart)).json();
    if (c && typeof c.text === 'string') { devLogText = c.text + devLogText; devLogStart = c.start || 0; }
    devRenderLog(false);
  } catch (err) {
    showToast(cmdError(err), true);
  } finally { devBusy = false; }
}

// ── ESP32 YENİDEN BAŞLATMA (Cihaz sekmesi, tek yazma komutu) ──
// Buton yalnız bağlantı (yerel/uzak) ve cihaz çevrimiçiyken aktif; onay penceresi sonrası POST /api/restart.
let restartBusy = false;

function updateRestartBtn() {
  const b = document.getElementById('dev-restart-btn');
  if (!b) return;
  const online = (connMode === 'local' || connMode === 'remote') && !restartBusy;
  b.disabled = !online;
  b.title = online ? '' : (restartBusy ? 'Yeniden başlatılıyor…' : 'Cihaz çevrim dışıyken kullanılamaz');
}

function restartAsk() {
  if (restartBusy || !requireConnection()) return;
  document.getElementById('restart-dlg').hidden = false;
}

function restartCancel() {
  const d = document.getElementById('restart-dlg');
  if (d) d.hidden = true;
}

async function restartConfirm() {
  restartCancel();
  if (restartBusy) return;
  restartBusy = true; updateRestartBtn();
  const ok = await sendCommand(ESP32_IP + '/api/restart', () => showToast('Yeniden başlatılıyor, cihaz birkaç dakika çevrim dışı görünecek', !1, !0));
  if (ok) setTimeout(() => { restartBusy = false; updateRestartBtn(); }, 15000); // yeniden başlama sürerken çift istek olmasın
  else { restartBusy = false; updateRestartBtn(); }
}

// Bağlantı yokken komut/sorgu göndermeden uyarı ver. true = bağlantı var.
// silent=true: uyarı gösterme (arka plan sorguları için).
function requireConnection(silent) {
  const m = Transport.mode();
  if (m === 'local' || m === 'remote') return true;
  if (!silent) showToast(m === 'device-offline' ? 'Cihaz çevrimdışı' : m === 'noconfig' ? 'Bağlantı ayarı yok (başlığa dokunun)' : m === 'connecting' ? 'Bağlanıyor…' : m === 'auth-error' ? 'Kimlik hatası, ayarları kontrol edin' : 'Bağlantı yok', true);
  return false;
}

function cmdError(err) {
  const m = err && err.message;
  if (m === 'timeout') return 'Cihaz yanıt vermedi (zaman aşımı)';
  if (m === 'device_offline') return 'Cihaz çevrimdışı';
  if (m === 'offline') return 'Bağlantı yok';
  if (m === 'connection_lost') return 'Bağlantı koptu';
  return 'Komut gönderilemedi';
}

// Komutu cihaza gönderir; cihazdan onay (200 + ok) gelmeden state/arayüz değişmez.
// onOk(gövde): onay gelince çağrılır. el: basılan öğe (beklerken çift basışı engeller, soluklatır).
async function sendCommand(url, onOk, el, body) {
  if (!requireConnection()) return false;
  if (el && el.dataset.busy) return false;
  if (el) { el.dataset.busy = '1'; el.style.opacity = '.55'; }
  try {
    const opts = { method: 'POST' };
    if (body !== undefined) opts.body = JSON.stringify(body);
    const res = await apiFetch(url, opts);
    const d = await res.json().catch(() => ({}));
    if (res.ok && !(d && d.ok === false)) { onOk(d); return true; }
    showToast(d && d.reason === 'sleeping' ? 'Cihaz uykuda 💤' : (res.status === 504 || (d && d.err === 'timeout')) ? 'Mama kabına ulaşılamadı (zaman aşımı)' : 'Komut cihaz tarafından reddedildi', true);
    return false;
  } catch (err) {
    showToast(cmdError(err), true);
    return false;
  } finally {
    if (el) { delete el.dataset.busy; el.style.opacity = ''; }
  }
}

function updateDashMiniIndicators() {
  // --- 1. AYDINLATMA GÖSTERGELERİ ---
  const miniLightsEl = document.getElementById('mini-lights');
  if (miniLightsEl) {
    let lightHtml = '';
    state.lights.forEach((isOn) => {
      if (isOn) {
        lightHtml += `
          <svg class="dash-mini-icon mini-icon-bulb" viewBox="0 0 24 24" fill="currentColor">
            <path d="M11.27 3a6 6 0 0 0-3.53 10.22A4.13 4.13 0 0 1 9 16.12.5.5 0 0 0 9 16h6a.5.5 0 0 0 0 .12 4 4 0 0 1 1.19-2.83A6 6 0 0 0 11.27 3"/>
            <path d="M16.19 13.29A4 4 0 0 0 15 16.12V19a1 1 0 0 1-1 1h-4a1 1 0 0 1-1-1v-2.87a4.13 4.13 0 0 0-1.26-2.91 6 6 0 1 1 8.45.07M9 16a.6.6 0 0 1 0 .13V19a1 1 0 0 0 1 1h4a1 1 0 0 0 1-1v-2.88a.5.5 0 0 1 0-.12Zm4 4h-2v1h2Z" style="fill:none;stroke:#000;stroke-linecap:round;stroke-linejoin:round;stroke-width:1.5"/>
          </svg>`;
      }
    });
    miniLightsEl.innerHTML = lightHtml;
  }

  // --- 2. SULAMA VE HAVUZ GÖSTERGELERİ ---
  const miniWaterEl = document.getElementById('mini-water');
  if (miniWaterEl) {
    let waterHtml = '';
    state.valves.forEach((v) => {
      if (v.isOpen) {
        waterHtml += `
          <svg class="dash-mini-icon mini-icon-solenoid" viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
            <g stroke="#2c3e50" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round">
              <path fill="#8e83a3" d="M5 47h20v16H5zm70 0h20v16H75z"/>
              <rect x="23" y="42" width="10" height="26" rx="3" fill="#e2edf8"/>
              <rect x="67" y="42" width="10" height="26" rx="3" fill="#e2edf8"/>
              <path d="M33 52h10V35h14c1 0 0 0 0 17h10v8H33Z" fill="#8e83a3"/>
              <rect x="37.5" y="29" width="25" height="8" rx="3" fill="#e2edf8"/>
              <path d="M35 76s-6 10-6 14c0 3.3 2.7 6 6 6s6-2.7 6-6c0-4-6-14-6-14m30 0s-6 10-6 14c0 3.3 2.7 6 6 6s6-2.7 6-6c0-4-6-14-6-14" fill="#4f8ef7"/>
              <path d="m52 35-8 14h4l-4 15 12-17h-6Z" fill="#f0c040" stroke-width="1" stroke-linejoin="miter"/>
            </g>
          </svg>`;
      }
    });
    if (state.poolOn) {
      waterHtml += `
        <svg class="dash-mini-icon mini-icon-pump" viewBox="0 0 24 24">
          <rect x="7" y="3" width="9" height="10" rx="1" fill="#3dba74"/>
          <path d="M10 3V1.5a1.5 1.5 0 0 1 3 0V3" fill="none" stroke="#3dba74" stroke-width="1.5" stroke-linecap="round"/>
          <rect x="7" y="12.5" width="9" height="3" rx=".3" fill="#7a7f96"/>
          <rect x="7" y="14.5" width="1.5" height="2" rx=".5" fill="#7a7f96"/>
          <rect x="10.75" y="14.5" width="1.5" height="2" rx=".5" fill="#7a7f96"/>
          <rect x="14.5" y="14.5" width="1.5" height="2" rx=".5" fill="#7a7f96"/>
        </svg>`
    }
    miniWaterEl.innerHTML = waterHtml;
  }
}

function toggleLight(li) {
  const el = document.getElementById('light-' + li), next = !state.lights[li];
  sendCommand(`${ESP32_IP}/api/light?id=${li}&on=${next}`, () => {
    state.lights[li] = next;
    el.classList.toggle('is-on', next);
    updateDashMiniIndicators();
    showToast(`Bölge ${li+1} ${next ? 'açıldı' : 'kapatıldı'}`);
  }, el);
}

function toggleValveManual(vi) {
  const next = !state.valves[vi].isOpen, el = document.getElementById((vi === 0 ? 'v1' : 'v2') + '-manual');
  sendCommand(`${ESP32_IP}/api/valve-on-off?id=${vi}&on=${next}`, () => {
    state.valves[vi].isOpen = next;
    updateValveUI(vi);
    updateDashMiniIndicators();
    showToast(`Vana ${vi+1} ${next ? 'açıldı' : 'kapatıldı'}`);
  }, el);
}

function toggleValveAuto(vi) {
  const next = !state.valves[vi].autoEnabled, el = document.getElementById((vi === 0 ? 'v1' : 'v2') + '-auto');
  sendCommand(`${ESP32_IP}/api/valve-auto-enable?id=${vi}&on=${next}`, () => {
    state.valves[vi].autoEnabled = next;
    updateValveUI(vi);
    showToast(`Vana ${vi+1} otomasyon ${next ? 'açıldı' : 'kapatıldı'}`);
  }, el);
}

function updateValveUI(vi) {
  const t = state.valves[vi],
    a = vi === 0 ? 'v1' : 'v2';
  document.getElementById(a + '-manual')
    .classList.toggle('is-on', t.isOpen), document.getElementById(a + '-manual-status')
    .textContent = t.isOpen ? 'AÇIK' : 'KAPALI',
  document.getElementById(a + '-auto')
    .classList.toggle('is-on', t.autoEnabled), document.getElementById(a + '-auto-status')
    .textContent = t.autoEnabled ? 'AKTİF' : 'KAPALI'
}

function attachLongPress(e, t, a) {
  let l, o, n;
  const s = e.querySelector('.lp-fill');

  function d(t) {
    t.preventDefault(), o = Date.now(), n = !1, s.style.transition = 'none',
    s.style.height = '0%', e.classList.add('filling'), void s.offsetWidth,
    s.style.transition = `height ${LONG_MS}ms linear`, s.style.height = '100%',
    l = setTimeout(() => { n = !0, i(), a() }, LONG_MS)
  }

  function i() {
    clearTimeout(l), e.classList.remove('filling'), s.style.transition = 'none',
    s.style.height = '0%', n || Date.now() - o < SHORT_MS && t()
  }
  e.addEventListener('mousedown', d), e.addEventListener('mouseup', i), e.addEventListener('mouseleave', i),
  e.addEventListener('touchstart', d, { passive: !1 }), e.addEventListener('touchend', i)
}

attachLongPress(document.getElementById('btn-pool'),
  () => showToast('Motoru açmak/kapatmak için basılı tutun', !1, !0),
  () => {
    const el = document.getElementById('btn-pool'), next = !state.poolOn;
    sendCommand(`${ESP32_IP}/api/pool-on-off?on=${next}`, () => {
      state.poolOn = next;
      el.classList.toggle('is-on', next);
      document.getElementById('pool-status').textContent = `${next ? 'AÇIK' : 'KAPALI'} — basılı tutarak aç/kapat`;
      updateDashMiniIndicators();
      showToast(`Havuz motoru ${next ? 'açıldı' : 'kapatıldı'}`);
    }, el);
}),

attachLongPress(document.getElementById('v1-auto'),
  () => showPage('vcfg-0'),
  () => toggleValveAuto(0)
),

attachLongPress(document.getElementById('v2-auto'),
  () => showPage('vcfg-1'),
  () => toggleValveAuto(1)
);

function fetchValveCfg(vi) {
  if (!requireConnection()) { renderValveCfg(vi); return; }
  apiFetch(`${ESP32_IP}/api/valve-cfg?id=${vi}`)
    .then(r => r.json())
    .then(data => {
      // API'den gelen slotları ve diğer ayarları state'e aktarıyoruz
      if (data.slots) {
        state.valves[vi].slots = data.slots;
      }
      if (data.autoEnabled !== undefined) {
        state.valves[vi].autoEnabled = data.autoEnabled;
      }
      if (data.manualTimeout !== undefined) {
        state.valves[vi].manualTimeout = data.manualTimeout;
      }
      // Veriler güncellendikten sonra sayfayı render et
      renderValveCfg(vi);
    })
    .catch(() => {
      showToast(`Vana ${vi + 1} ayarları alınamadı`, !0);
      // Bağlantı hatası olsa bile mevcut state ile arayüzü çiz
      renderValveCfg(vi);
    });
}

function renderValveCfg(e) {
  const t = state.valves[e], l = e => String(e).padStart(2, '0'), a = document.getElementById('vcfg-slots-' + e);
  a.innerHTML = '', t.slots.forEach((t, o) => {
      a.innerHTML +=
        `<div class="slot-row"><div class="slot-num">${o+1}</div>
        <div class="slot-fields">
          <input class="time-in" type="number" id="w${e}-h-${o}" value="${l(t.h)}" min="0" max="23">
          <span class="slot-sep2">:</span>
          <input class="time-in" type="number" id="w${e}-m-${o}" value="${l(t.m)}" min="0" max="59">
          <div class="slot-divider"></div>
          <input class="dur-in" type="number" id="w${e}-d-${o}" value="${t.dur}" min="1" max="60">
          <span class="slot-unit">dk</span></div>
          <label class="toggle">
            <input type="checkbox" id="w${e}-a-${o}" ${t.active?'checked':''}>
            <div class="toggle-track"></div>
          </label>
        </div>`
    }),
    document.getElementById('vcfg-auto-' + e).checked = t.autoEnabled,
    document.getElementById('v' + e + '-timeout').value = t.manualTimeout,
    document.getElementById('conflict-' + e).classList.remove('show')
}

function checkConflicts(e) {
  const t = e.map(e => ({
      label: e.label, start: 60 * e.h + e.m, end: 60 * e.h + e.m + e.dur
    })),
    a = [];
  for (let e = 0; e < t.length; e++)
    for (let l = e + 1; l < t.length; l++) {
      const o = t[e], n = t[l];
      o.start < n.end && n.start < o.end && a.push(`${o.label} ↔ ${n.label}`)
    }
  return a
}

let cfgSaving = false;
function saveValveCfg(e) {
  if (!requireConnection()) return;
  const t = state.valves[e];
  let a = !0;
  const l = t.slots.map((t, l) => {
      const o = parseInt(document.getElementById(`w${e}-h-${l}`).value),
        n = parseInt(document.getElementById(`w${e}-m-${l}`).value),
        s = parseInt(document.getElementById(`w${e}-d-${l}`).value),
        d = document.getElementById(`w${e}-a-${l}`).checked;
      (isNaN(o) || o < 0 || o > 23 || isNaN(n) || n < 0 || n > 59 || isNaN(s) || s < 1 || s > 60) && (a = !1);
      return {
        h: o, m: n, dur: s, active: d
      }
    }),
    n = document.getElementById('vcfg-auto-' + e).checked,
    o = parseInt(document.getElementById('v' + e + '-timeout').value);
  if (!a || isNaN(o) || o < 1) return void showToast('Geçersiz değer', !0);
  const s = e === 0 ? 1 : 0,
    d = [...l.filter(e => e.active)
      .map((t, a) => ({
        label: `Vana ${e+1} Slot ${a+1}`, h: t.h, m: t.m, dur: t.dur
      })), ...state.valves[s].slots.filter(e => e.active)
      .map((e, t) => ({
        label: `Vana ${s+1} Slot ${t+1}`, h: e.h, m: e.m, dur: e.dur
      }))
    ],
    i = checkConflicts(d),
    r = document.getElementById('conflict-' + e);
  if (i.length) return r.classList.add('show'),
    r.innerHTML = '⚠️ Çakışma tespit edildi:<br>' + i.join('<br>'),
    void showToast( 'Zaman çakışması var', !0);
  r.classList.remove('show');
  if (cfgSaving) return;
  cfgSaving = true;
  sendCommand(`${ESP32_IP}/api/valve-cfg`, () => {
    t.slots = l; t.manualTimeout = o; t.autoEnabled = n; updateValveUI(e);
    showToast(`Vana ${e+1} ayarları kaydedildi`);
    setTimeout(() => showPage('water'), 800);
  }, null, { vi: e, slots: l, manualTimeout: o, autoEnabled: n }).finally(() => { cfgSaving = false; });
}

// Feeder önbellek verisi: hem /api/feeder/status cevabı hem de durum push'undaki `feeder` nesnesi buraya gelir
function applyFeederData(d) {
  if (!d || d.battery === undefined) return;
  state.feeder.batPct = d.battery;
  state.feeder.foodPct = d.foodLevel;
  state.feeder.isSleeping = d.sleep;
  state.feeder.unreachable = !!d.unreachable;
  updateFeederUI();
  updateFeederMini(d.battery, d.foodLevel);
  updateFeederSleepVisuals(d.sleep, !!d.unreachable);
}

// Mama Kabı paneline girince ESP32'den feeder'ı sorgulamasını ister (ana panelde yapılmaz). Cevap hemen döner;
// yeni değerler / 'ulaşılamıyor' bayrağı durum push'u ile gelir (Transport.onStatus -> applyFeederData).
function refreshFeederCache() {
  if (!requireConnection(true)) return;
  apiFetch(ESP32_IP + '/api/feeder/refresh', { method: 'POST' }).catch(() => {});
}

function fetchFeederStatus(silent) {
  if (!requireConnection(silent)) return;
  apiFetch(ESP32_IP + '/api/feeder/status')
    .then(r => r.json()).then(applyFeederData)
    .catch(() => { if (!silent) showToast('Mama kabı ile iletişim kurulamadı', !1, !0); });
}

function fetchFeederConfig() {
  if (!requireConnection(true)) return; // uyarıyı fetchFeederStatus verir
  apiFetch(`${ESP32_IP}/api/feeder/config`).then(r => r.json()).then(d => {
    if (d.slots) state.feeder.slots = d.slots;
    if (d) {
        state.feeder.autoEnabled  = d.autoFeedEnable || false;
        state.feeder.sleep.active = d.deepSleepEnable || false;
        state.feeder.sleep.startH = d.startH || 0;
        state.feeder.sleep.startM = d.startM || 0;
        state.feeder.sleep.endH   = d.endH || 0;
        state.feeder.sleep.endM   = d.endM || 0;
    }
    updateFeedAutoBtn(); updateNextMeal();
  }).catch(() => showToast('Feeder ayarları tam olarak alınamadı', !0));
}

function fetchFeederTime() {
  if (!requireConnection()) {
    const el = document.getElementById('feeder-cfg-time');
    if (el) el.textContent = '!';
    return;
  }
  apiFetch(`${ESP32_IP}/api/feeder/time`)
    .then(r => r.json()).then(d => {
      const el = document.getElementById('feeder-cfg-time');
      if (el) el.textContent = (d.ok && d.time) ? d.time : '?';
    })
    .catch(() => {
      const el = document.getElementById('feeder-cfg-time');
      if (el) el.textContent = '!';
    });
}

function updateFeederUI() {
  const e = state.feeder,
    t = Math.max(0, Math.min(100, e.batPct || 0)),
    a = t <= 20 ? '#e05c5c' : '#3dba74',
    l = Math.round(.44 * t);
  document.getElementById('bat-fill')
    .setAttribute('width', l), document
    .getElementById('bat-fill')
    .setAttribute('fill', a), document
    .getElementById('bat-pct')
    .innerHTML = `${t}<span>%</span>`;
  const o = Math.max(0, Math.min(100, e.foodPct || 0)),
    n = o <= 20 ? '#e05c5c' : '#3dba74';
  document.getElementById('food-fill')
    .style.height = o + '%', document
    .getElementById('food-fill')
    .style.background = n, document.getElementById(
      'food-pct')
    .innerHTML = `${o}<span>%</span>`, updateFeedAutoBtn(), updateNextMeal()
}

function updateFeederMini(e, t) {
  const a = e <= 20 ? '#e05c5c' : '#3dba74',
    l = t <= 20 ? '#e05c5c' : '#3dba74',
    o = Math.round(Math.max(0, Math.min(100, e)) / 100 * 21);
  document.getElementById('mini-bat-fill')
    .setAttribute('width', o), document
    .getElementById('mini-bat-fill')
    .setAttribute('fill', a), document
    .getElementById('mini-bat-pct')
    .textContent = e + '%', document
    .getElementById('mini-food-fill')
    .style.width = t + '%', document
    .getElementById('mini-food-fill')
    .style.background = l, document
    .getElementById('mini-food-pct')
    .textContent = t + '%'
}

function updateFeederSleepVisuals(isSleeping, unreachable) {
  const dashBadge = document.getElementById('feeder-sleep-badge');
  const pageBadge = document.getElementById('feeder-page-sleep-badge');
  const feederPage = document.getElementById('page-feeder');
  const show = (isSleeping || unreachable) ? 'block' : 'none';

  if (dashBadge) {
    dashBadge.style.display = show;
    dashBadge.textContent = unreachable ? '⚠ Ulaşılamıyor' : '💤 Uykuda';
    dashBadge.classList.toggle('unreachable', !!unreachable);
  }
  if (pageBadge) {
    pageBadge.style.display = show;
    pageBadge.textContent = unreachable ? '⚠ Cihaz Ulaşılamıyor' : '💤 Cihaz Uykuda';
    pageBadge.classList.toggle('unreachable', !!unreachable);
  }
  if (feederPage) feederPage.classList.toggle('feeder-sleeping', !!isSleeping && !unreachable);
}

fetchFeederStatus(true);

function triggerFeed() {
  const e = document.getElementById('btn-feed');
  sendCommand(ESP32_IP + '/api/feeder/feed', () => {
    showToast('Yemleme tetiklendi');
    setTimeout(() => fetchFeederStatus(true), 3000);
  }, e);
}
const BTN_AUTO_FEED = document.getElementById('btn-auto-feed'),
  LP_FILL_FEED = document.getElementById('lp-fill-feed');
let lpTimerF = null, lpStartF = 0, lpFiredF = !1;

function startLPF(e) {
  e.preventDefault(), lpStartF = Date.now(), lpFiredF = !1, LP_FILL_FEED.style
    .transition = 'none', LP_FILL_FEED.style.height = '0%', BTN_AUTO_FEED
    .classList.add('filling'), void LP_FILL_FEED.offsetWidth, LP_FILL_FEED.style
    .transition = `height ${LONG_MS}ms linear`, LP_FILL_FEED.style.height =
    '100%', lpTimerF = setTimeout(() => {
      lpFiredF = !0, endLPF(), toggleFeedAuto()
    }, LONG_MS)
}

function endLPF(e) {
  clearTimeout(lpTimerF), BTN_AUTO_FEED.classList.remove('filling'),
    LP_FILL_FEED.style.transition = 'none', LP_FILL_FEED.style.height = '0%',
    lpFiredF || Date.now() - lpStartF < SHORT_MS && showPage('feeder-config')
}
BTN_AUTO_FEED.addEventListener('mousedown', startLPF),
BTN_AUTO_FEED.addEventListener('mouseup', endLPF),
BTN_AUTO_FEED.addEventListener('mouseleave', endLPF),
BTN_AUTO_FEED.addEventListener('touchstart', startLPF, { passive: !1 }),
BTN_AUTO_FEED.addEventListener('touchend', endLPF);

function toggleFeedAuto() {
  const next = !state.feeder.autoEnabled;
  sendCommand(`${ESP32_IP}/api/feeder/config`, () => {
    state.feeder.autoEnabled = next;
    updateFeedAutoBtn(); updateNextMeal();
    showToast(`Otomatik yemleme ${next ? 'açıldı' : 'kapatıldı'}`);
  }, BTN_AUTO_FEED, { autoFeedEnable: next, slots: state.feeder.slots });
}

function updateFeedAutoBtn() {
  BTN_AUTO_FEED.classList.toggle('is-on', state.feeder.autoEnabled),
  document.getElementById('feed-auto-status').textContent =
    state.feeder.autoEnabled ? 'AKTİF' : 'KAPALI'
}

function updateNextMeal() {
  if (!state.feeder.autoEnabled) return void(document.getElementById('next-meal')
    .textContent = 'Otomasyon kapalı');
  const e = new Date,
    t = 60 * e.getHours() + e.getMinutes(),
    a = state.feeder.slots.filter(e => e.active);
  if (!a.length) return void(document.getElementById('next-meal').textContent =
    'Slot tanımlı değil');
  const l = [...a].map(e => ({
      ...e,
      total: 60 * e.h + e.m
    }))
    .sort((e, t) => e.total - t.total),
    o = l.find(e => e.total > t) || l[0],
    n = e => String(e)
    .padStart(2, '0');
  document.getElementById('next-meal')
    .textContent = `${n(o.h)}:${n(o.m)}`
}

function renderFeedConfig() {
  fetchFeederTime();
  const e = document.getElementById('feed-slots-container'),
    t = e => String(e).padStart(2, '0');

  e.innerHTML = '';
  state.feeder.slots.forEach((a, l) => {
    e.innerHTML +=
      `<div class="slot-row">
        <div class="slot-num">${l+1}</div>
        <div class="slot-time">
          <input type="number" id="sl-h-${l}" value="${t(a.h)}" min="0" max="23">
          <span class="slot-sep">:</span>
          <input type="number" id="sl-m-${l}" value="${t(a.m)}" min="0" max="59">
        </div>
        <label class="toggle">
          <input type="checkbox" id="sl-a-${l}" ${a.active ? 'checked' : ''}>
          <div class="toggle-track"></div>
        </label>
      </div>`
  });

  document.getElementById('cfg-auto-toggle').checked = state.feeder.autoEnabled;
  document.getElementById('cfg-sleep-toggle').checked = state.feeder.sleep.active;
  document.getElementById('slp-sh').value = t(state.feeder.sleep.startH);
  document.getElementById('slp-sm').value = t(state.feeder.sleep.startM);
  document.getElementById('slp-eh').value = t(state.feeder.sleep.endH);
  document.getElementById('slp-em').value = t(state.feeder.sleep.endM);
}

let feedCfgSaving = false;
function saveFeedConfig() {
  if (!requireConnection()) return;
  let isValid = true;

  const slots = state.feeder.slots.map((t, a) => {
    const l = parseInt(document.getElementById('sl-h-' + a).value),
          o = parseInt(document.getElementById('sl-m-' + a).value),
          n = document.getElementById('sl-a-' + a).checked;
    if (isNaN(l) || l < 0 || l > 23 || isNaN(o) || o < 0 || o > 59) isValid = false;
    return { h: l, m: o, active: n };
  });

  const sh = parseInt(document.getElementById('slp-sh').value),
        sm = parseInt(document.getElementById('slp-sm').value),
        eh = parseInt(document.getElementById('slp-eh').value),
        em = parseInt(document.getElementById('slp-em').value);
  if (isNaN(sh) || sh < 0 || sh > 23 || isNaN(eh) || eh < 0 || eh > 23 ||
      isNaN(sm) || sm < 0 || sm > 59 || isNaN(em) || em < 0 || em > 59) isValid = false;

  if (!isValid) return showToast('Geçersiz saat değeri', true);
  if (feedCfgSaving) return;

  // State, cihazdan onay gelene kadar değişmez
  const autoEnabled = document.getElementById('cfg-auto-toggle').checked;
  const sleep = { active: document.getElementById('cfg-sleep-toggle').checked, startH: sh, startM: sm, endH: eh, endM: em };

  feedCfgSaving = true;
  sendCommand(`${ESP32_IP}/api/feeder/config`, () => {
    state.feeder.slots = slots;
    state.feeder.autoEnabled = autoEnabled;
    state.feeder.sleep = sleep;
    updateFeedAutoBtn(); updateNextMeal();
    showToast('Ayarlar kaydedildi');
    setTimeout(() => showPage('feeder'), 800);
  }, null, {
    slots,
    autoFeedEnable: autoEnabled,
    deepSleepEnable: sleep.active,
    startH: sleep.startH,
    startM: sleep.startM,
    endH: sleep.endH,
    endM: sleep.endM
  }).finally(() => { feedCfgSaving = false; });
}

let statusClock = null, connMode = 'connecting';

function applyStatus(d) {
  if (d.time) {
    const p = d.time.split(':').map(Number);
    statusClock = { s: p[0] * 3600 + p[1] * 60 + p[2], at: Date.now(), valid: d.timeValid, ntp: d.timeSyncedByNTP };
  }
  try {
    {
      d.lights && d.lights.forEach((l, i) => {
        state.lights[i] = l,
        document.getElementById('light-' + i).classList.toggle('is-on', l)
      }),
      d.valves && d.valves.forEach((v, i) => {
        state.valves[i].isOpen = v.isOpen,
        state.valves[i].autoEnabled = v.autoEnabled,
        updateValveUI(i)
      }),
      d.poolOn !== undefined && (
        state.poolOn = d.poolOn,
        document.getElementById('btn-pool').classList.toggle('is-on', d.poolOn),
        document.getElementById('pool-status').textContent = (d.poolOn ? 'AÇIK' : 'KAPALI') + ' — basılı tutarak aç/kapat'
      ),
      updateDashMiniIndicators()
    }
    ;applyFeederData(d.feeder);
    renderClock();
  } catch (e) {}
}

// Başlık saati: ESP32 saati 10 sn'de bir gelir, aradaki saniyeleri burada sayarız.
function renderClock() {
  updateRestartBtn();
  const el = document.getElementById('header-time');
  if (!el) return;
  const txt = document.getElementById('header-time-text') || el;
  const set = (text, color) => { txt.textContent = text; el.style.color = color; };
  el.classList.toggle('noconfig', connMode === 'noconfig');
  el.title = connLongStatus();
  if (connMode === 'noconfig') return set('Yapılandırma yok', '#4f8ef7');
  if (connMode === 'connecting') return set('Bağlanıyor…', '#7a7f96');
  if (connMode === 'auth-error') return set('Kimlik hatası', '#e05c5c');
  if (connMode === 'offline') return set('Bağlantı yok', '#e05c5c');
  if (connMode === 'device-offline') return set('☁ Cihaz çevrimdışı', '#e05c5c');
  if (!statusClock) return set(connMode === 'remote' ? '☁ Veri bekleniyor' : 'Veri bekleniyor', '#7a7f96');
  const s = (statusClock.s + Math.floor((Date.now() - statusClock.at) / 1000)) % 86400;
  const t = [Math.floor(s / 3600), Math.floor(s % 3600 / 60), s % 60].map(n => String(n).padStart(2, '0')).join(':');
  set((statusClock.valid ? '' : '⚠ ') + t + (connMode === 'remote' ? ' ☁' : ''), statusClock.valid ? '#3dba74' : '#e05c5c');
}

// Başlık butonundaki kısa durumun uzun açıklaması (ayar panelindeki durum ekranında ve buton ipucunda gösterilir).
function connLongStatus() {
  if (connMode === 'noconfig') return 'MQTT veya yerel WS ayarı girilmemiş. Bağlanmak için MQTT sekmesini doldurun.';
  if (connMode === 'connecting') return 'Bağlantı kuruluyor…';
  if (connMode === 'auth-error') return 'MQTT kullanıcı adı veya şifresi reddedildi. Ayarı düzeltip kaydedin.';
  if (connMode === 'offline') return 'Sunucuya ve cihaza ulaşılamıyor. İnternet bağlantısını kontrol edin.';
  if (connMode === 'device-offline') return 'MQTT sunucusuna bağlı, ancak ESP32 çevrimdışı görünüyor.';
  const via = connMode === 'remote' ? 'Uzaktan (MQTT) bağlı.' : 'Yerel ağdan bağlı.';
  if (!statusClock) return via + ' Cihazdan ilk durum verisi bekleniyor.';
  if (!statusClock.valid) return via + ' Cihaz saati güvenilmez, sulama askıda.';
  return via + ' Cihaz saati ' + (statusClock.ntp ? 'NTP' : 'RTC') + ' ile senkron.';
}

Transport.onStatus(applyStatus);
Transport.onState(m => {
  const wasOnline = connMode === 'local' || connMode === 'remote';
  connMode = m; renderClock();
  // Bağlantı kurulunca mama kabı bilgisini çek (açılışta WS henüz açık olmadığı için ana panel göstergeleri boş kalıyordu)
  if ((m === 'local' || m === 'remote') && !wasOnline) fetchFeederStatus(true);
});
Transport.start();
setInterval(renderClock, 1000);

// Başlıktaki bağlantı durumuna dokununca ayar paneli açılır (MQTT sekmesi, tanılama dahil)
const headerTimeEl = document.getElementById('header-time');
if (headerTimeEl) headerTimeEl.addEventListener('click', () => showConfig('mqtt'));
// Ayar kaydedilince bağlantı yeni ayarla yeniden kurulur (Transport.reload adım 5'te eklenecek)
Config.onChange(() => { if (typeof Transport.reload === 'function') Transport.reload(); });

let toastTimer;

function showToast(e, t = !1, a = !1) {
  const l = document.getElementById('toast');
  l.textContent = e, l.className = 'toast' + (t ? ' error' : a ? ' info' : ''),
    void l.offsetWidth, l.classList.add('show'), clearTimeout(toastTimer),
    toastTimer = setTimeout(() => l.classList.remove('show'), 2400)
}

function openCamera() {
  if (window.Android) Android.openCamera();
  else showToast('Web sürümünde kamera erişilemiyor', !1, !0)
}