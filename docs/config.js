/* Ortak bağlantı ayarları (MQTT | WS | NVR). App ve webde aynı sayfa.
 * App: ayarlar Android köprüsü ile şifreli saklanır. Web: yalnız MQTT, localStorage'da.
 * Bu dosya transport.js'ten ÖNCE yüklenir. Ayar değişince Config.onChange dinleyicileri çağrılır. */

const IS_APP = !!window.Android;

const Config = (() => {
  const LS_KEY = 'cfg_mqtt';
  const DEFAULT_ROOT = 'smarthome/panel';
  const cbs = [];
  let data = {
    mqtt: { url: '', user: '', pass: '', root: DEFAULT_ROOT },
    ws: { ip: '', port: 81 },
    nvr: { name: '', ip: '', port: 554, username: '', hasPassword: false }
  };

  function load() {
    try {
      if (IS_APP) {
        const o = JSON.parse(Android.getConfig());
        data.mqtt = Object.assign({}, data.mqtt, o.mqtt || {});
        data.ws = Object.assign({}, data.ws, o.ws || {});
        data.nvr = Object.assign({}, data.nvr, o.nvr || {});
      } else {
        const s = localStorage.getItem(LS_KEY);
        if (s) data.mqtt = Object.assign({}, data.mqtt, JSON.parse(s));
      }
    } catch (e) {}
    if (!data.mqtt.root) data.mqtt.root = DEFAULT_ROOT;
  }

  function notify() { cbs.forEach(cb => { try { cb(); } catch (e) {} }); }

  // Boş url = MQTT yapılandırması yok (kayıt silinir)
  function saveMqtt(o) {
    const m = {
      url: (o.url || '').trim(),
      user: (o.user || '').trim(),
      pass: o.pass || '',
      root: (o.root || '').trim().replace(/^\/+|\/+$/g, '') || DEFAULT_ROOT
    };
    if (m.url) {
      if (!/^wss?:\/\/\S+$/i.test(m.url)) return { ok: false, err: 'Adres wss:// ile başlamalı' };
      if (!m.user || !m.pass) return { ok: false, err: 'Kullanıcı adı ve şifre gerekli' };
    }
    try {
      if (IS_APP) { if (!Android.saveMqtt(JSON.stringify(m))) return { ok: false, err: 'Kaydedilemedi' }; }
      else localStorage.setItem(LS_KEY, JSON.stringify(m));
    } catch (e) { return { ok: false, err: 'Kaydedilemedi' }; }
    data.mqtt = m;
    notify();
    return { ok: true };
  }

  // Boş ip = WS yapılandırması yok. Yalnız app.
  function saveWs(o) {
    if (!IS_APP) return { ok: false, err: 'Web sürümünde kullanılamaz' };
    const ip = (o.ip || '').trim();
    const port = parseInt(o.port, 10) || 81;
    if (ip && !/^[A-Za-z0-9.\-]+$/.test(ip)) return { ok: false, err: 'Geçersiz adres' };
    if (port < 1 || port > 65535) return { ok: false, err: 'Geçersiz port' };
    try {
      if (!Android.saveWs(JSON.stringify({ ip, port }))) return { ok: false, err: 'Kaydedilemedi' };
    } catch (e) { return { ok: false, err: 'Kaydedilemedi' }; }
    data.ws = { ip, port };
    notify();
    return { ok: true };
  }

  load();

  return {
    reload: load,
    get: () => data,
    mqtt: () => (data.mqtt.url ? { url: data.mqtt.url, user: data.mqtt.user, pass: data.mqtt.pass, root: data.mqtt.root } : null),
    ws: () => (IS_APP && data.ws.ip ? { url: 'ws://' + data.ws.ip + ':' + data.ws.port } : null),
    saveMqtt, saveWs,
    onChange: cb => cbs.push(cb),
    notify
  };
})();

// ── Ayar paneli arayüzü ──
const _cfgEl = id => document.getElementById(id);
let _cfgDiagTimer = null, _cfgNvrBusy = false;

function cfgTab(tab) {
  document.querySelectorAll('#cfg-tabs .tab-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === tab));
  document.querySelectorAll('.cfg-pane').forEach(p => p.classList.toggle('active', p.id === 'cfg-pane-' + tab));
  if (tab === 'device' && typeof devRefresh === 'function') devRefresh();
}

function _cfgLock(paneId, locked) {
  const pane = _cfgEl(paneId);
  pane.classList.toggle('locked', locked);
  pane.querySelectorAll('input, button.save-btn').forEach(el => { el.disabled = locked; });
  const note = pane.querySelector('.cfg-lock-note');
  if (note) note.style.display = locked ? 'block' : 'none';
}

function cfgFill() {
  const d = Config.get();
  _cfgEl('cfg-mqtt-url').value = d.mqtt.url;
  _cfgEl('cfg-mqtt-user').value = d.mqtt.user;
  _cfgEl('cfg-mqtt-pass').value = d.mqtt.pass;
  _cfgEl('cfg-mqtt-root').value = d.mqtt.root;
  _cfgEl('cfg-ws-ip').value = d.ws.ip;
  _cfgEl('cfg-ws-port').value = d.ws.port;
  _cfgEl('cfg-nvr-name').value = d.nvr.name;
  _cfgEl('cfg-nvr-ip').value = d.nvr.ip;
  _cfgEl('cfg-nvr-port').value = d.nvr.port;
  _cfgEl('cfg-nvr-user').value = d.nvr.username;
  _cfgEl('cfg-nvr-pass').value = '';
  _cfgEl('cfg-nvr-pass').placeholder = d.nvr.hasPassword ? 'Kayıtlı (değiştirmek için yazın)' : 'Şifre';
  _cfgEl('cfg-nvr-delete').style.display = IS_APP && d.nvr.ip ? 'block' : 'none';
  _cfgLock('cfg-pane-ws', !IS_APP);
  _cfgLock('cfg-pane-nvr', !IS_APP);
  _cfgBusy(_cfgNvrBusy);
}

function _cfgBusy(busy) {
  _cfgNvrBusy = busy;
  const b = _cfgEl('cfg-nvr-save');
  if (!b) return;
  b.textContent = busy ? 'Bağlanılıyor…' : 'Bağlan ve Kaydet';
  if (IS_APP) { b.disabled = busy; _cfgEl('cfg-nvr-delete').disabled = busy; }
}

function _cfgDiagDraw() {
  const el = _cfgEl('cfg-diag');
  if (el) el.textContent = (typeof connLongStatus === 'function' ? 'Durum: ' + connLongStatus() + '\n\n' : '') + ((typeof Transport !== 'undefined' && Transport.diag) ? Transport.diag() : '');
}

function _cfgDiagStart() {
  _cfgDiagDraw();
  clearInterval(_cfgDiagTimer);
  _cfgDiagTimer = setInterval(() => {
    const p = _cfgEl('page-config');
    if (!p || !p.classList.contains('active')) { clearInterval(_cfgDiagTimer); return; }
    _cfgDiagDraw();
  }, 1000);
}

function showConfig(tab) {
  Config.reload();
  cfgFill();
  cfgTab(tab || 'mqtt');
  showPage('config');
  _cfgDiagStart();
}
window.showConfig = showConfig;

function cfgSaveMqtt() {
  const r = Config.saveMqtt({
    url: _cfgEl('cfg-mqtt-url').value,
    user: _cfgEl('cfg-mqtt-user').value,
    pass: _cfgEl('cfg-mqtt-pass').value,
    root: _cfgEl('cfg-mqtt-root').value
  });
  if (r.ok) { cfgFill(); showToast(Config.mqtt() ? 'MQTT ayarı kaydedildi' : 'MQTT ayarı silindi'); }
  else showToast(r.err, true);
}

function cfgSaveWs() {
  const r = Config.saveWs({ ip: _cfgEl('cfg-ws-ip').value, port: _cfgEl('cfg-ws-port').value });
  if (r.ok) { cfgFill(); showToast(Config.ws() ? 'WS ayarı kaydedildi' : 'WS ayarı silindi'); }
  else showToast(r.err, true);
}

function cfgSaveNvr() {
  if (!IS_APP || _cfgNvrBusy) return;
  _cfgBusy(true);
  try {
    Android.saveNvr(JSON.stringify({
      name: _cfgEl('cfg-nvr-name').value,
      ip: _cfgEl('cfg-nvr-ip').value,
      port: parseInt(_cfgEl('cfg-nvr-port').value, 10) || 554,
      username: _cfgEl('cfg-nvr-user').value,
      pass: _cfgEl('cfg-nvr-pass').value
    }));
  } catch (e) { _cfgBusy(false); showToast('Kaydedilemedi', true); }
}

function cfgDeleteNvr() {
  if (!IS_APP || _cfgNvrBusy) return;
  try { Android.deleteNvr(); } catch (e) { showToast('Silinemedi', true); }
}

// Android köprüsü NVR işleminin sonucunu buraya bildirir
window.onNvrResult = function (ok, msg) {
  _cfgBusy(false);
  if (ok) { Config.reload(); cfgFill(); }
  showToast(msg, !ok);
};
