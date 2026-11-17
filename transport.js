/* Uygulama <-> ESP32 iletişimi.
 * Uzakta: EMQX üzerinden MQTT (wss). Yerelde (yalnız app): ESP32 WebSocket (ws://ip:port).
 * Adresler ve kimlik bilgileri koda gömülü değil, Config'ten (config.js) gelir.
 * Tek mesaj protokolü: istek {id,m,p,q,b} -> cevap {id,code,b}; ESP32 durumu {ev:'status',b} olarak iter.
 * apiFetch(url, opts) fetch() gibi davranır, mevcut script.js çağrıları bununla çalışır. */

// ── Minimal MQTT 3.1.1 istemcisi (WebSocket üzerinden; QoS 0, yalnızca gereken paketler) ──
const _enc = new TextEncoder();
const _dec = new TextDecoder();

function _utf(str) {
  const b = _enc.encode(str);
  const out = new Uint8Array(2 + b.length);
  out[0] = b.length >> 8; out[1] = b.length & 255; out.set(b, 2);
  return out;
}
function _concat(arrs) {
  let n = 0; arrs.forEach(a => n += a.length);
  const out = new Uint8Array(n); let o = 0;
  arrs.forEach(a => { out.set(a, o); o += a.length; });
  return out;
}
function _remLen(n) {
  const out = [];
  do { let d = n % 128; n = Math.floor(n / 128); if (n > 0) d |= 128; out.push(d); } while (n > 0);
  return out;
}
function _packet(first, body) {
  const rl = _remLen(body.length);
  const out = new Uint8Array(1 + rl.length + body.length);
  out[0] = first; out.set(rl, 1); out.set(body, 1 + rl.length);
  return out;
}

class MqttLite {
  constructor(url, clientId, user, pass, cb) {
    this.url = url; this.clientId = clientId; this.user = user; this.pass = pass; this.cb = cb;
    this.buf = new Uint8Array(0); this.sock = null; this.dead = false; this.opened = false; this.subTopics = [];
    this.pid = 0; this.lastRx = Date.now(); this.pingTimer = null; this.connTimer = null;
  }
  connect() {
    let s;
    try { s = new WebSocket(this.url, 'mqtt'); } catch (e) { this._fail('WebSocket oluşturulamadı: ' + e.message); return; }
    s.binaryType = 'arraybuffer'; this.sock = s;
    this.connTimer = setTimeout(() => this._fail(this.opened ? 'CONNACK gelmedi (8 sn)' : 'WSS bağlantısı kurulamadı (8 sn zaman aşımı)'), 8000);
    s.onopen = () => {
      this.opened = true;
      const head = new Uint8Array([0, 4, 77, 81, 84, 84, 4, 0xC2, 0, 30]); // MQTT, v3.1.1, user+pass+clean, keepalive 30 sn
      this._send(_packet(0x10, _concat([head, _utf(this.clientId), _utf(this.user), _utf(this.pass)])));
    };
    s.onmessage = (ev) => { this.lastRx = Date.now(); this._onData(new Uint8Array(ev.data)); };
    s.onerror = () => {}; // hata olayını ardından gelen close izler
    s.onclose = (ev) => {
      if (this.sock !== s) return;
      this._fail((this.opened ? 'bağlantı kapandı' : 'WSS bağlantısı kurulamadı (adres/port/path/TLS/ağ)') + (ev && ev.code ? ' [kod ' + ev.code + ']' : ''));
    };
  }
  subscribe(topics) {
    this.subTopics = topics;
    const parts = [new Uint8Array([++this.pid >> 8 & 255, this.pid & 255])];
    topics.forEach(t => { parts.push(_utf(t)); parts.push(new Uint8Array([0])); });
    this._send(_packet(0x82, _concat(parts)));
  }
  publish(topic, str) {
    if (!this.sock || this.sock.readyState !== 1) throw new Error('mqtt_closed');
    this._send(_packet(0x30, _concat([_utf(topic), _enc.encode(str)])));
  }
  close() {
    this.dead = true; clearInterval(this.pingTimer); clearTimeout(this.connTimer);
    const s = this.sock; this.sock = null;
    if (s) { s.onopen = s.onmessage = s.onclose = s.onerror = null; try { s.send(new Uint8Array([0xE0, 0])); } catch (e) {} try { s.close(); } catch (e) {} }
  }
  // code: 'auth' = kullanıcı adı/şifre hatalı veya yetkisiz (yeniden denemek anlamsız)
  _fail(reason, code) {
    if (this.dead) return;
    this.close();
    if (this.cb.onClose) this.cb.onClose(reason || 'bilinmeyen sebep', code);
  }
  _send(u8) { if (this.sock && this.sock.readyState === 1) this.sock.send(u8); }
  _onData(chunk) {
    const b = new Uint8Array(this.buf.length + chunk.length); b.set(this.buf); b.set(chunk, this.buf.length); this.buf = b;
    while (this.buf.length >= 2) {
      let mul = 1, len = 0, i = 1, d;
      do {
        if (i >= this.buf.length) return;
        d = this.buf[i++]; len += (d & 127) * mul; mul *= 128;
      } while ((d & 128) && i < 5);
      if (this.buf.length < i + len) return;
      const type = this.buf[0] >> 4, flags = this.buf[0] & 15;
      const body = this.buf.slice(i, i + len);
      this.buf = this.buf.slice(i + len);
      this._onPacket(type, flags, body);
      if (this.dead) return;
    }
  }
  _onPacket(type, flags, body) {
    if (type === 2) { // CONNACK
      if (body[1] === 0) {
        clearTimeout(this.connTimer);
        this.pingTimer = setInterval(() => this._send(new Uint8Array([0xC0, 0])), 15000);
        if (this.cb.onConnect) this.cb.onConnect();
      } else this._fail('CONNACK reddedildi: ' + ({ 1: 'protokol sürümü', 2: 'clientId', 3: 'sunucu kullanılamıyor', 4: 'kullanıcı adı/şifre hatalı', 5: 'yetkisiz' }[body[1]] || 'kod ' + body[1]), (body[1] === 4 || body[1] === 5) ? 'auth' : 'refused');
    } else if (type === 3) { // PUBLISH
      const tl = (body[0] << 8) | body[1];
      const topic = _dec.decode(body.slice(2, 2 + tl));
      let off = 2 + tl;
      if ((flags >> 1) & 3) off += 2;
      if (this.cb.onMessage) this.cb.onMessage(topic, _dec.decode(body.slice(off)));
    } else if (type === 9) { // SUBACK: 0x80 = ACL tarafından reddedildi
      const denied = [];
      for (let i = 2; i < body.length; i++) if (body[i] === 0x80) denied.push(this.subTopics[i - 2] || ('#' + (i - 2)));
      if (this.cb.onSubAck) this.cb.onSubAck(denied);
    }
  }
}

// ── Taşıyıcı ──
// Modlar: noconfig (MQTT ve WS ayarı yok), connecting (ilk deneme sürüyor), offline, device-offline
// (sunucuya bağlı, cihaz çevrimdışı), remote (MQTT), local (WS), auth-error (MQTT kimlik hatası).
// Açılışta MQTT hemen denenir. App'te yerel WS arka planda yoklanır; WS açılırsa MQTT kesilir.
const Transport = (() => {
  const REQ_TIMEOUT_MS = 4000;           // istek başına zaman aşımı
  const STALE_MS = 25000;                // ESP32 10 sn'de bir durum yollar; 25 sn sessizlik = ölü bağlantı
  const WS_POLL_MQTT_UP_MS = 60000;      // MQTT bağlıyken yerel WS yoklama aralığı
  const MQTT_AFTER_WS_DROP_MS = 2500;    // yerel bağlantı koptuktan bu kadar sonra MQTT'ye geç (kısa kopmalarda gidip gelmesin)

  const statusCbs = [], stateCbs = [];
  const pending = new Map();
  let cfgMqtt = Config.mqtt(), cfgWs = Config.ws();
  let seq = 0, started = false;

  let ws = null, wsUp = false, wsLastMsg = 0, wsRetry = 0, wsTimer = null, wsFailed = false;
  let wsConnStart = 0, wsDownSince = 0;

  let mq = null, mqUp = false, deviceOnline = null, mqRetry = 0, mqRetryAt = 0, mqRoot = '';
  let mqFailed = false, mqAuthFail = false;
  const diag = {
    ws: cfgWs ? 'başlamadı' : (IS_APP ? 'yapılandırma yok' : 'web sürümünde kullanılamaz'),
    wsTries: 0,
    mq: cfgMqtt ? 'başlamadı' : 'yapılandırma yok',
    mqErr: '', mqSubDenied: ''
  };

  function computeMode() {
    if (wsUp) return 'local';
    if (!cfgMqtt && !cfgWs) return 'noconfig';
    if (cfgMqtt && mqAuthFail) return 'auth-error';
    if (mqUp) return deviceOnline === true ? 'remote' : 'device-offline';
    if (cfgMqtt) return mqFailed ? 'offline' : 'connecting';
    return wsFailed ? 'offline' : 'connecting';
  }
  let mode = computeMode();

  function updateMode() {
    const m = computeMode();
    if (m !== mode) { mode = m; stateCbs.forEach(cb => { try { cb(mode); } catch (e) {} }); }
  }

  function failPending(via) {
    pending.forEach((p, id) => {
      if (p.via === via) { clearTimeout(p.timer); pending.delete(id); p.reject(new Error('connection_lost')); }
    });
  }

  function handleMessage(text) {
    let o;
    try { o = JSON.parse(text); } catch (e) { return; }
    if (o.ev === 'status') { statusCbs.forEach(cb => { try { cb(o.b); } catch (e) {} }); return; }
    const p = o.id && pending.get(o.id);
    if (p) { clearTimeout(p.timer); pending.delete(o.id); p.resolve({ code: o.code, b: o.b }); }
  }

  // ── yerel WebSocket (yalnız app, ayar varsa) ──
  function wsConnect() {
    clearTimeout(wsTimer);
    wsDrop();
    if (!cfgWs) return;
    let sock;
    try { sock = new WebSocket(cfgWs.url); } catch (e) { wsScheduleRetry(); return; }
    ws = sock; wsConnStart = Date.now(); diag.wsTries++; diag.ws = 'bağlanıyor';
    sock.onopen = () => {
      if (ws !== sock) return;
      wsUp = true; wsRetry = 0; wsFailed = false; wsLastMsg = Date.now(); diag.ws = 'bağlı';
      mqttStop(); updateMode();
    };
    sock.onmessage = (ev) => { wsLastMsg = Date.now(); handleMessage(ev.data); };
    sock.onclose = sock.onerror = () => { if (ws !== sock) return; wsDrop(); wsScheduleRetry(); };
  }
  function wsDrop() {
    const s = ws; ws = null;
    if (s) { s.onopen = s.onmessage = s.onclose = s.onerror = null; try { s.close(); } catch (e) {} }
    if (wsUp) { wsUp = false; wsDownSince = Date.now(); failPending('ws'); updateMode(); }
  }
  function wsScheduleRetry() {
    clearTimeout(wsTimer);
    wsFailed = true;
    if (!cfgWs) { updateMode(); return; }
    // MQTT bağlıyken yerel WS yalnızca ara sıra yoklanır; değilken hızlı yeniden dener
    const delay = mqUp ? WS_POLL_MQTT_UP_MS : Math.min(1000 * Math.pow(2, wsRetry), 10000); wsRetry++;
    diag.ws = 'bağlanamadı, yeniden denenecek (' + Math.round(delay / 1000) + ' sn)';
    wsTimer = setTimeout(wsConnect, delay);
    updateMode();
  }
  // MQTT bağlantısı koptuğunda yerel WS'yi hemen yokla
  function wsKick() {
    if (cfgWs && !wsUp) { wsRetry = 0; wsConnect(); }
  }

  // ── MQTT (uzak) ──
  function mqttStart() {
    if (mq || !cfgMqtt || mqAuthFail || wsUp) return;
    if (Date.now() < mqRetryAt) return;
    mqUp = false; deviceOnline = null; diag.mq = 'bağlanıyor'; diag.mqSubDenied = '';
    const root = mqRoot = cfgMqtt.root;
    const client = new MqttLite(cfgMqtt.url, 'app_' + Math.random().toString(16).slice(2, 10), cfgMqtt.user, cfgMqtt.pass, {
      onConnect: () => {
        if (mq !== client) return;
        mqUp = true; mqFailed = false; mqRetry = 0; diag.mq = 'bağlı'; diag.mqErr = '';
        client.subscribe([root + '/res', root + '/status', root + '/online']);
        updateMode();
      },
      onMessage: (topic, payload) => {
        if (mq !== client) return;
        if (topic === root + '/online') {
          const was = deviceOnline;
          deviceOnline = (payload === 'online'); updateMode();
          // Cihaz çevrimiçi olunca durumu iste (ESP32 kuruluysa hemen dolsun)
          if (deviceOnline && was !== true) request('GET', '/api/status').then(r => statusCbs.forEach(cb => { try { cb(r.b); } catch (e) {} })).catch(() => {});
        }
        else handleMessage(payload);
      },
      onSubAck: (denied) => { if (mq === client) diag.mqSubDenied = denied.join(', '); },
      onClose: (why, code) => {
        if (mq !== client) return;
        mqFailed = true; diag.mq = 'hata'; diag.mqErr = why || '';
        if (code === 'auth') { mqAuthFail = true; diag.mq = 'kimlik hatası, yeniden deneme durdu (ayarı düzeltip kaydedin)'; }
        mqttDrop(!mqAuthFail);
      }
    });
    mq = client; client.connect();
  }
  function mqttDrop(penalty) {
    const c = mq; mq = null;
    if (c) c.close();
    const was = mqUp; mqUp = false; deviceOnline = null;
    failPending('mqtt');
    if (penalty) { mqRetry++; mqRetryAt = Date.now() + Math.min(2000 * Math.pow(2, mqRetry), 60000); }
    updateMode();
    if (penalty && was) wsKick();
  }
  function mqttStop() { if (mq) diag.mq = 'kapalı (yerel bağlantı var)'; mqttDrop(false); }

  // ── istek ──
  function request(method, path, q, b) {
    return new Promise((resolve, reject) => {
      const via = wsUp ? 'ws' : (mqUp ? 'mqtt' : null);
      if (!via) { reject(new Error('offline')); return; }
      if (via === 'mqtt' && deviceOnline !== true) { reject(new Error('device_offline')); return; }
      const id = 'r' + (++seq).toString(36) + Math.random().toString(36).slice(2, 6);
      const msg = JSON.stringify({ id, m: method, p: path, q: q || {}, b });
      const timer = setTimeout(() => { pending.delete(id); reject(new Error('timeout')); }, REQ_TIMEOUT_MS);
      pending.set(id, { resolve, reject, timer, via });
      try {
        if (via === 'ws') ws.send(msg); else mq.publish(mqRoot + '/req', msg);
      } catch (e) { clearTimeout(timer); pending.delete(id); reject(e); }
    });
  }

  // ── bekçi: ölü bağlantıları temizler, gerekirse MQTT'ye geçer ──
  function tick() {
    const now = Date.now();
    if (ws && !wsUp && now - wsConnStart > 3000) { wsDrop(); wsScheduleRetry(); }
    if (wsUp && now - wsLastMsg > STALE_MS) { wsDrop(); wsScheduleRetry(); }
    if (!wsUp && now - wsDownSince > MQTT_AFTER_WS_DROP_MS) mqttStart();
    if (mq && mqUp && now - mq.lastRx > 45000) mqttDrop(true);
  }

  // Ayar değişince her şeyi yeni ayarla baştan kur
  function reload() {
    cfgMqtt = Config.mqtt(); cfgWs = Config.ws();
    mqAuthFail = false; mqFailed = false; wsFailed = false;
    mqRetry = 0; mqRetryAt = 0; wsRetry = 0; wsDownSince = 0; diag.mqErr = '';
    diag.mq = cfgMqtt ? 'başlamadı' : 'yapılandırma yok';
    diag.ws = cfgWs ? 'başlamadı' : (IS_APP ? 'yapılandırma yok' : 'web sürümünde kullanılamaz');
    clearTimeout(wsTimer);
    wsDrop();
    mqttDrop(false);
    updateMode();
    if (started) { if (cfgWs) wsConnect(); tick(); }
  }

  // Ön plana dönüş / ağ değişimi: bağlantıları hemen sına, yerel WS'yi yokla
  function probeNow() {
    mqRetryAt = 0;
    if (!started) return;
    if (cfgWs) {
      if (wsUp && Date.now() - wsLastMsg <= 12000) { /* sağlıklı */ }
      else { wsRetry = 0; wsConnect(); }
    }
    if (mq && mqUp && Date.now() - mq.lastRx > 20000) mqttDrop(true);
    tick();
  }

  function start() {
    if (started) return;
    started = true;
    if (cfgWs) wsConnect();
    tick();
    setInterval(tick, 1000);
    document.addEventListener('visibilitychange', () => { if (!document.hidden) probeNow(); });
    window.addEventListener('online', probeNow);
    if (navigator.connection && navigator.connection.addEventListener) navigator.connection.addEventListener('change', probeNow);
  }

  function diagText() {
    const L = [];
    L.push('Mod: ' + mode);
    L.push('Yerel WS: ' + diag.ws + (diag.wsTries ? ' (deneme ' + diag.wsTries + ')' : ''));
    let mqs = diag.mq;
    if (cfgMqtt && !mq && !mqAuthFail && Date.now() < mqRetryAt) mqs = 'yeniden denemede (' + Math.ceil((mqRetryAt - Date.now()) / 1000) + ' sn)';
    L.push('MQTT: ' + mqs);
    if (diag.mqErr) L.push('Son MQTT hatası: ' + diag.mqErr);
    if (mqUp) {
      L.push('Abonelik reddi (ACL): ' + (diag.mqSubDenied || 'yok'));
      L.push('Cihaz: ' + (deviceOnline === null ? 'henüz haber yok' : deviceOnline ? 'çevrimiçi' : 'çevrimdışı'));
    }
    return L.join('\n');
  }

  return {
    start, request, reload,
    onStatus: cb => statusCbs.push(cb),
    onState: cb => { stateCbs.push(cb); cb(mode); },
    mode: () => mode,
    diag: diagText
  };
})();

// fetch() yerine geçer: URL'yi {yol, sorgu} olarak ayrıştırıp Transport üzerinden gönderir.
// fetch gibi, ağ hatasında reddeder; sunucu hata kodu (504 vb.) dönerse çözülür.
function apiFetch(url, opts) {
  opts = opts || {};
  const u = new URL(url);
  const q = {};
  u.searchParams.forEach((v, k) => { q[k] = v; });
  let b;
  if (opts.body) { try { b = JSON.parse(opts.body); } catch (e) {} }
  return Transport.request((opts.method || 'GET').toUpperCase(), u.pathname, q, b)
    .then(r => ({ ok: r.code >= 200 && r.code < 300, status: r.code, json: () => Promise.resolve(r.b) }));
}
