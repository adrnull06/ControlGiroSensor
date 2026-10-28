
const CONFIG = {
  
  USE_MOCK: false,

  // Vacío a propósito: la clave no se publica en el código; la web la pide al abrir y la guarda en el navegador.
  API_KEY: '',

  POLL_MS: 500,

 
  INTEGRANTES: [
    { apellido: 'Salmerón',   foto: 'img/salmeroni.jpg' },
    { apellido: 'Leandro', foto: 'img/leandro.jpg' },
    { apellido: 'Castillo', foto: 'img/castillo.jpg' },
    { apellido: '', foto: 'img/fabri.jpg' },
  ],
};

const qs = new URLSearchParams(location.search).get('mock');
if (qs === '1') CONFIG.USE_MOCK = true;
if (qs === '0') CONFIG.USE_MOCK = false;

/* ============================================================
   CAPA DE DATOS: API real o simulada (misma interfaz)
   Devuelve siempre { status, data }
   ============================================================ */
const store = {
  get(k) { try { return localStorage.getItem(k); } catch { return null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch { /* sin storage */ } },
  del(k) { try { localStorage.removeItem(k); } catch { /* sin storage */ } },
};

function getKey() {
  if (CONFIG.API_KEY) return CONFIG.API_KEY;
  let k = store.get('apiKey');
  if (!k) {
    k = (prompt('Ingresá la API key del backend:') || '').trim();
    if (k) store.set('apiKey', k);
  }
  return k;
}

async function realApi(method, path, body) {
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), 4000);
  try {
    const res = await fetch(path, {
      method,
      headers: { 'Content-Type': 'application/json', 'X-API-Key': getKey() || '' },
      body: body ? JSON.stringify(body) : undefined,
      signal: ctrl.signal,
    });
    let data = {};
    try { data = await res.json(); } catch { /* sin cuerpo */ }
    if (res.status === 401) store.del('apiKey'); // clave mala: se vuelve a pedir
    return { status: res.status, data };
  } finally {
    clearTimeout(t);
  }
}

// --- Simulador: imita al backend y al Arduino ---
const sim = { distance: 30, dir: 1, motor: 'stop', mode: 'auto', command: 'stop' };

function simStep() {
  sim.distance += sim.dir * (0.8 + Math.random() * 1.6);
  if (sim.distance > 48) sim.dir = -1;
  if (sim.distance < 6) sim.dir = 1;
  if (sim.mode === 'auto') {
    const d = sim.distance;
    sim.motor = d >= 10 && d <= 25 ? 'right' : d > 25 && d <= 40 ? 'left' : 'stop';
  } else {
    sim.motor = sim.command;
  }
}

async function mockApi(method, path, body) {
  await new Promise(r => setTimeout(r, 40 + Math.random() * 60));
  if (method === 'GET' && path === '/api/status') {
    simStep();
    return { status: 200, data: { distance: +sim.distance.toFixed(1), motor: sim.motor, mode: sim.mode, online: true } };
  }
  if (method === 'POST' && path === '/api/mode') {
    if (!['auto', 'manual'].includes(body.mode)) return { status: 400, data: { error: 'mode inválido' } };
    sim.mode = body.mode; sim.command = 'stop';
    return { status: 200, data: { mode: sim.mode } };
  }
  if (method === 'POST' && path === '/api/motor') {
    if (!['left', 'right', 'stop'].includes(body.command)) return { status: 400, data: { error: 'command inválido' } };
    if (sim.mode !== 'manual') return { status: 409, data: { error: 'El modo actual es automático; cambiá a manual primero' } };
    sim.command = body.command;
    return { status: 200, data: { command: sim.command } };
  }
  return { status: 404, data: { error: 'Ruta no encontrada' } };
}

const api = (m, p, b) => (CONFIG.USE_MOCK ? mockApi(m, p, b) : realApi(m, p, b));

/* INTERFAZ */
const $ = id => document.getElementById(id);
const el = {
  distance: $('distance'), marker: $('marker'),
  motorState: $('motorState'), motorText: $('motorText'), motorIcon: $('motorIcon'),
  online: $('online'), onlineText: $('onlineText'),
  btnAuto: $('btnAuto'), btnManual: $('btnManual'), modeHint: $('modeHint'),
  ctls: [...document.querySelectorAll('.ctl')],
  msg: $('msg'),
};

const MOTOR_LABEL = { left: 'Girando a la izquierda', right: 'Girando a la derecha', stop: 'Detenido' };
const MOTOR_ICON = {
  left: '<path d="M15 5l-8 7 8 7"/><path d="M21 12H8"/>',
  right: '<path d="M9 5l8 7-8 7"/><path d="M3 12h13"/>',
  stop: '<rect x="6" y="6" width="12" height="12" rx="1.5"/>',
};

let current = { mode: null, motor: null, online: null };
let msgTimer;

function showMsg(text, ok = false) {
  el.msg.textContent = text;
  el.msg.classList.toggle('ok', ok);
  clearTimeout(msgTimer);
  if (text) msgTimer = setTimeout(() => { el.msg.textContent = ''; }, 3500);
}

function renderOnline(online, reachable = true) {
  el.online.className = 'badge ' + (online && reachable ? 'on' : 'off');
  el.onlineText.textContent = !reachable
    ? 'Sin conexión con el servidor'
    : online ? 'Dispositivo en línea' : 'Dispositivo desconectado';
}

function renderDistance(d) {
  if (typeof d !== 'number') {
    el.distance.textContent = '--';
    el.marker.classList.remove('vis');
    return;
  }
  el.distance.textContent = d.toFixed(1);
  el.marker.style.left = Math.min(100, Math.max(0, (d / 50) * 100)) + '%';
  el.marker.classList.add('vis');
}

function renderMotor(motor) {
  const m = MOTOR_LABEL[motor] ? motor : 'stop';
  el.motorState.className = 'motor-state ' + m;
  el.motorText.textContent = MOTOR_LABEL[m];
  el.motorIcon.innerHTML = MOTOR_ICON[m];
}

function renderMode(mode) {
  const manual = mode === 'manual';
  el.btnAuto.classList.toggle('active', !manual);
  el.btnManual.classList.toggle('active', manual);
  el.btnAuto.setAttribute('aria-pressed', String(!manual));
  el.btnManual.setAttribute('aria-pressed', String(manual));
  el.ctls.forEach(b => { b.disabled = !manual; });
  el.modeHint.textContent = manual
    ? 'El motor responde solo a los botones. El sensor sigue midiendo.'
    : 'El sensor controla el motor. Pasá a manual para usar los botones.';
}

function renderStatus(s) {
  renderOnline(s.online);
  renderDistance(s.distance);
  renderMotor(s.motor);
  if (s.mode !== current.mode) renderMode(s.mode);
  current = { mode: s.mode, motor: s.motor, online: s.online };
  el.ctls.forEach(b => b.classList.toggle('sel', current.mode === 'manual' && b.dataset.cmd === s.motor));
}

/* ---------- Polling cada 500 ms (sin solaparse) ---------- */
async function poll() {
  try {
    const { status, data } = await api('GET', '/api/status');
    if (status === 200) renderStatus(data);
    else if (status === 401) { renderOnline(false, false); showMsg('API key inválida'); }
    else renderOnline(false, false);
  } catch {
    renderOnline(false, false);
  }
  setTimeout(poll, CONFIG.POLL_MS);
}

/* ---------- Acciones ---------- */
async function send(path, body, okText) {
  try {
    const { status, data } = await api('POST', path, body);
    if (status === 200) { if (okText) showMsg(okText, true); return true; }
    if (status === 409) showMsg('Cambiá a modo manual para mover el motor.');
    else if (status === 401) showMsg('API key inválida. Recargá la página para ingresarla de nuevo.');
    else showMsg(data.error || 'Error ' + status);
  } catch {
    showMsg('No se pudo contactar al servidor.');
  }
  return false;
}

[el.btnAuto, el.btnManual].forEach(b => b.addEventListener('click', async () => {
  const mode = b.dataset.mode;
  if (mode === current.mode) return;
  if (await send('/api/mode', { mode })) {
    current.mode = mode;
    renderMode(mode);
  }
}));

el.ctls.forEach(b => b.addEventListener('click', async () => {
  const command = b.dataset.cmd;
  if (await send('/api/motor', { command })) {
    el.ctls.forEach(x => x.classList.toggle('sel', x === b));
  }
}));

/* ---------- Equipo + imágenes con respaldo ---------- */
function buildTeam() {
  const grid = $('teamGrid');
  CONFIG.INTEGRANTES.forEach(p => {
    const card = document.createElement('div');
    card.className = 'member';
    const img = document.createElement('img');
    img.className = 'ph';
    img.src = p.foto;
    img.alt = 'Foto de ' + p.apellido;
    img.onerror = () => {
      const ph = document.createElement('div');
      ph.className = 'ph missing';
      ph.textContent = p.apellido.trim().charAt(0).toUpperCase();
      img.replaceWith(ph);
    };
    const name = document.createElement('b');
    name.textContent = p.apellido;
    card.append(img, name);
    grid.append(card);
  });
}

$('escudo').addEventListener('error', e => e.target.classList.add('missing'));

/* ---------- Arranque ---------- */
if (CONFIG.USE_MOCK) $('mockBanner').hidden = false;
buildTeam();
renderMode('auto');
renderMotor('stop');
poll();
