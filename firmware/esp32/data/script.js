const $ = id => document.getElementById(id);
let selectedSSID = '';
let mqttHasPass = false;
let mqttSnap = null;   // yüklenen MQTT ayarı: {host, port, user}
let apSnap = null;     // yüklenen AP ayarı: {ssid, pass, ip}
let mqttBusy = false;
let apBusy = false;
let statusBusy = false;
let toastTimer;

// ── Yardımcılar ───────────────────────────────────────────────
function setMsg(text, kind) {
  const el = $('msg');
  el.textContent = text;
  el.className = 'msg show ' + (kind || '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = 'msg'; }, 6000);
}

function postJson(url, body) {
  return fetch(url, {
    method:  'POST',
    headers: { 'Content-Type': 'application/json' },
    body:    JSON.stringify(body)
  }).then(r => r.json());
}

function isValidIP(ip) {
  return /^((25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(25[0-5]|2[0-4]\d|[01]?\d\d?)$/.test(ip);
}

function setTab(name) {
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.pane').forEach(p => p.classList.toggle('active', p.id === 'pane-' + name));
}

// ── Kaydet butonları: yalnız değişiklik varken aktif ──────────
function mqttDirty() {
  if (!mqttSnap) return false;
  const port = parseInt($('mqttPort').value.trim() || '8883', 10);
  return $('mqttHost').value.trim() !== mqttSnap.host ||
         $('mqttUser').value.trim() !== mqttSnap.user ||
         port !== mqttSnap.port ||
         $('mqttPass').value !== '';
}

function apDirty() {
  if (!apSnap) return false;
  return $('apSSID').value.trim() !== apSnap.ssid ||
         $('apPass').value !== apSnap.pass ||
         $('apIP').value.trim() !== apSnap.ip;
}

function updateButtons() {
  $('wifiSaveBtn').disabled = !selectedSSID;
  $('mqttSaveBtn').disabled = mqttBusy || !mqttDirty();
  $('saveApBtn').disabled   = apBusy || !apDirty();
}

document.addEventListener('input', updateButtons);

// ── Wi-Fi: ağ tarama ──────────────────────────────────────────
function scanNetworks() {
  const list = $('netList');
  list.innerHTML = '<div class="scanning">Ağlar taranıyor...</div>';
  return fetch('/api/scan')
    .then(r => r.json())
    .then(d => {
      list.textContent = '';
      if (!d.networks || !d.networks.length) {
        list.innerHTML = '<div class="scanning">Ağ bulunamadı</div>';
        return;
      }
      d.networks.forEach(n => {
        const item = document.createElement('div');
        item.className = 'net-item' + (n.ssid === selectedSSID ? ' selected' : '');
        const name = document.createElement('span');
        name.className = 'net-name';
        name.textContent = n.ssid;
        const rssi = document.createElement('span');
        rssi.className = 'net-rssi';
        const bars = n.rssi > -60 ? '▂▄▆█' : n.rssi > -75 ? '▂▄▆' : n.rssi > -85 ? '▂▄' : '▂';
        rssi.textContent = bars + (n.open ? '' : ' 🔒');
        item.appendChild(name);
        item.appendChild(rssi);
        item.onclick = () => selectNet(item, n.ssid);
        list.appendChild(item);
      });
    })
    .catch(() => {
      list.innerHTML = '<div class="scanning">Tarama hatası</div>';
    });
}

function selectNet(el, ssid) {
  document.querySelectorAll('.net-item').forEach(e => e.classList.remove('selected'));
  el.classList.add('selected');
  selectedSSID = ssid;
  updateButtons();
  $('wifiPass').focus();
}

function saveWifi() {
  if (!selectedSSID) return;
  setMsg('Ayarlar kaydediliyor...');
  postJson('/api/wifi-config', { ssid: selectedSSID, pass: $('wifiPass').value })
    .then(d => {
      if (d.ok) setMsg('Kaydedildi. Cihaz ağa bağlanıyor...', 'ok');
      else      setMsg('Hata: ' + (d.err || 'bilinmiyor'), 'err');
    })
    .catch(() => setMsg('Bağlantı hatası', 'err'));
}

// ── MQTT ──────────────────────────────────────────────────────
function loadMqtt() {
  return fetch('/api/mqtt-config')
    .then(r => r.json())
    .then(d => {
      const port = d.port || 8883;
      $('mqttHost').value = d.host || '';
      $('mqttPort').value = port;
      $('mqttUser').value = d.user || '';
      mqttHasPass = !!d.hasPass;
      $('mqttPass').value = '';
      $('mqttPass').placeholder = mqttHasPass ? 'Kayıtlı (değiştirmek için yazın)' : 'Şifre';
      mqttSnap = { host: d.host || '', port: port, user: d.user || '' };
      updateButtons();
    })
    .catch(() => {});
}

function saveMqtt() {
  const host = $('mqttHost').value.trim();
  const user = $('mqttUser').value.trim();
  const pass = $('mqttPass').value;
  const port = parseInt($('mqttPort').value.trim() || '8883', 10);

  if (/[\/:\s]/.test(host)) { setMsg('Yalnız sunucu adını yazın (örn. xxx.emqxsl.com), http/wss ve port olmadan', 'err'); return; }
  if (!(port >= 1 && port <= 65535)) { setMsg('Geçerli bir port girin', 'err'); return; }

  if (!host) {
    if (!confirm('Sunucu boş: MQTT ayarı silinecek ve uzaktan erişim kapanacak. Devam edilsin mi?')) return;
  } else {
    if (!user) { setMsg('Kullanıcı adı gerekli', 'err'); return; }
    if (!pass && !mqttHasPass) { setMsg('Şifre gerekli', 'err'); return; }
  }

  mqttBusy = true;
  updateButtons();
  const done = () => { mqttBusy = false; loadMqtt(); };
  postJson('/api/mqtt-config', { host, port, user, pass })
    .then(d => {
      if (d.ok) {
        setMsg('Kaydedildi. Cihaz yeniden başlıyor, yaklaşık 15 sn sonra sayfayı yenileyin.', 'ok');
        setTimeout(done, 15000);
      } else {
        mqttBusy = false;
        updateButtons();
        setMsg('Hata: ' + (d.err || 'bilinmiyor'), 'err');
      }
    })
    .catch(() => {
      // Cihaz yanıttan hemen sonra yeniden başladığı için bağlantı kopabilir.
      setMsg('Bağlantı kesildi. Cihaz yeniden başlıyor olabilir, birkaç saniye sonra sayfayı yenileyip kontrol edin.', 'err');
      setTimeout(done, 15000);
    });
}

// ── Cihaz: AP ayarları ────────────────────────────────────────
function loadAp() {
  return fetch('/api/network-config')
    .then(r => r.json())
    .then(d => {
      $('apSSID').value = d.apSSID || '';
      $('apPass').value = d.apPass || '';
      $('apIP').value   = d.apIP   || '';
      apSnap = { ssid: d.apSSID || '', pass: d.apPass || '', ip: d.apIP || '' };
      updateButtons();
    })
    .catch(() => {});
}

function saveAp() {
  const ssid = $('apSSID').value.trim();
  const pass = $('apPass').value;
  const ip   = $('apIP').value.trim();

  if (!ssid)                              { setMsg('AP adı boş bırakılamaz', 'err'); return; }
  if (pass.length > 0 && pass.length < 8) { setMsg('Şifre en az 8 karakter olmalı', 'err'); return; }
  if (!isValidIP(ip))                     { setMsg('Geçerli bir IP adresi girin', 'err'); return; }

  apBusy = true;
  updateButtons();
  postJson('/api/ap-config', { apSSID: ssid, apPass: pass, apIP: ip })
    .then(d => {
      if (d.ok) {
        apSnap = { ssid: ssid, pass: pass, ip: ip };
        setMsg('AP ayarları kaydedildi. Cihaz yeni IP (' + ip + ') ile yeniden başlatılıyor.', 'ok');
      } else {
        setMsg('Hata: ' + (d.err || 'bilinmiyor'), 'err');
      }
    })
    .catch(e => setMsg('Bağlantı hatası: ' + e, 'err'))
    .finally(() => { apBusy = false; updateButtons(); });
}

// ── Loglar ────────────────────────────────────────────────────
function getLogs() {
  window.open('/api/logs', '_blank');
}

function clearLogs() {
  if (!confirm('Tüm log kayıtlarını silmek istediğinize emin misiniz?')) return;
  fetch('/api/logs/clear', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      if (d.ok) setMsg('Log kayıtları temizlendi', 'ok');
      else      setMsg('Loglar temizlenirken hata oluştu', 'err');
    })
    .catch(() => setMsg('Bağlantı hatası, loglar temizlenemedi', 'err'));
}

// ── Durum ─────────────────────────────────────────────────────
function updateStatus() {
  if (statusBusy) return Promise.resolve();   // önceki istekler bitmeden yenisini başlatma
  statusBusy = true;

  const jobs = [];

  jobs.push(fetch('/api/reset-reason')
    .then(r => r.json())
    .then(d => { $('stReset').textContent = d.reason; })
    .catch(() => {}));

  jobs.push(fetch('/api/wifi-status')
    .then(r => r.json())
    .then(d => {
      const badge  = $('stBadge');
      const netRow = $('stNetRow');
      const ipRow  = $('stIpRow');
      if (d.connected) {
        badge.textContent = 'Bağlı';
        badge.className   = 'badge badge-ok';
        $('stNet').textContent = d.ssid;
        $('stIp').textContent  = d.ip;
        netRow.style.display = ipRow.style.display = 'flex';
      } else {
        badge.textContent = 'Bağlı Değil';
        badge.className   = 'badge badge-err';
        netRow.style.display = ipRow.style.display = 'none';
      }
    })
    .catch(() => {}));

  jobs.push(fetch('/api/mqtt-status')
    .then(r => r.json())
    .then(d => {
      const badge  = $('stMqtt');
      const detail = $('stMqttDetail');
      let text = '';
      if (!d.enabled) {
        badge.textContent = 'Kapalı';
        badge.className   = 'badge badge-warn';
        text = 'Ayar girilmemiş (MQTT sekmesi)';
      } else if (d.up) {
        badge.textContent = 'Bağlı';
        badge.className   = 'badge badge-ok';
      } else {
        badge.textContent = 'Bağlı Değil';
        badge.className   = 'badge badge-err';
        text = d.detail || 'Bağlanıyor...';
      }
      detail.textContent = text;
      detail.style.display = text ? 'block' : 'none';
    })
    .catch(() => {}));

  jobs.push(fetch('/api/status')
    .then(r => r.json())
    .then(d => {
      const el = $('stTime');
      if (d.timeValid) {
        el.textContent = d.time + (d.timeSyncedByNTP ? ' (NTP)' : ' (RTC)');
        el.style.color = '#3dba74';
      } else {
        el.textContent = d.time + ' (Sulama askıda!)';
        el.style.color = '#e05c5c';
      }
    })
    .catch(() => {}));

  return Promise.all(jobs).finally(() => { statusBusy = false; });
}

// ── Başlangıç ─────────────────────────────────────────────────
// Önce hızlı bilgiler (durum, MQTT, AP); Wi-Fi taraması cihazı birkaç saniye
// meşgul ettiği için en sona bırakılır.
updateButtons();
Promise.all([updateStatus(), loadMqtt(), loadAp()]).finally(scanNetworks);
setInterval(updateStatus, 3000);
