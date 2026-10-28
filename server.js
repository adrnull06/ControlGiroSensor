require('dotenv').config();
const express = require('express');
const path = require('path');

const app = express();
const PORT = process.env.PORT || 3000;
const API_KEY = process.env.API_KEY;

if (!API_KEY) {
  console.error('Falta API_KEY en las variables de entorno (.env)');
  process.exit(1);
}

// ---------- Constantes ----------
const MOTOR_COMMANDS = ['left', 'right', 'stop'];
const MODES = ['auto', 'manual'];
const ONLINE_TIMEOUT_MS = 5000; // si el Arduino no contacta en 5s => offline

// ---------- Estado en memoria ----------
const state = {
  distance: null,     // última distancia reportada por el Arduino
  motor: 'stop',      // estado real del motor reportado por el Arduino
  mode: 'auto',       // 'auto' | 'manual'
  command: 'stop',    // comando pendiente para el Arduino
  lastContact: null,  // timestamp (ms) del último sync del Arduino
};

// ---------- Middlewares ----------
app.use(express.json());

// Archivos estáticos de la web (no requieren clave)
app.use(express.static(path.join(__dirname, 'frontend-public')));

// Todas las rutas /api exigen X-API-Key
function requireApiKey(req, res, next) {
  if (req.get('X-API-Key') !== API_KEY) {
    return res.status(401).json({ error: 'API key inválida o ausente' });
  }
  next();
}
app.use('/api', requireApiKey);

// ---------- Rutas ----------

// Arduino: envía su estado y recibe modo + comando
app.post('/api/device/sync', (req, res) => {
  const { distance, motor } = req.body || {};

  if (typeof distance !== 'number' || !Number.isFinite(distance) || distance < 0) {
    return res.status(400).json({ error: 'distance debe ser un número >= 0' });
  }
  if (!MOTOR_COMMANDS.includes(motor)) {
    return res.status(400).json({ error: `motor debe ser uno de: ${MOTOR_COMMANDS.join(', ')}` });
  }

  state.distance = distance;
  state.motor = motor;
  state.lastContact = Date.now();

  res.json({ mode: state.mode, command: state.command });
});

// Web: consulta el estado (cada 500 ms)
app.get('/api/status', (req, res) => {
  const online =
    state.lastContact !== null && Date.now() - state.lastContact < ONLINE_TIMEOUT_MS;

  res.json({
    distance: state.distance,
    motor: state.motor,
    mode: state.mode,
    online,
  });
});

// Web: cambia el modo
app.post('/api/mode', (req, res) => {
  const { mode } = req.body || {};

  if (!MODES.includes(mode)) {
    return res.status(400).json({ error: `mode debe ser uno de: ${MODES.join(', ')}` });
  }

  state.mode = mode;
  state.command = 'stop'; // por seguridad, al cambiar de modo se frena el motor

  res.json({ mode: state.mode });
});

// Web: manda comando al motor (solo en modo manual)
app.post('/api/motor', (req, res) => {
  const { command } = req.body || {};

  if (!MOTOR_COMMANDS.includes(command)) {
    return res.status(400).json({ error: `command debe ser uno de: ${MOTOR_COMMANDS.join(', ')}` });
  }
  if (state.mode !== 'manual') {
    return res.status(409).json({ error: 'El modo actual es automático; cambiá a manual primero' });
  }

  state.command = command;
  res.json({ command: state.command });
});

// 404 para rutas /api desconocidas
app.use('/api', (req, res) => res.status(404).json({ error: 'Ruta no encontrada' }));

// Manejo de errores (ej. JSON malformado)
app.use((err, req, res, next) => {
  if (err.type === 'entity.parse.failed') {
    return res.status(400).json({ error: 'JSON inválido' });
  }
  console.error(err);
  res.status(500).json({ error: 'Error interno' });
});

app.listen(PORT, () => console.log(`API escuchando en puerto ${PORT}`));
