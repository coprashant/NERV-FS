'use strict';

require('dotenv').config(); 
const crypto = require('crypto');
const express = require('express');
const multer = require('multer');
const cors = require('cors');
const ngrok = require('@ngrok/ngrok');
const rateLimit = require('express-rate-limit');
const {
  listFiles,
  uploadFile,
  downloadFile,
  deleteFile,
  chmodFile,
} = require('./nervfsClient');

const app = express();
app.set('trust proxy', 1);
const upload = multer();

// Mandatory Security Environment Checks
const BRIDGE_PORT = Number(process.env.BRIDGE_PORT || 4000);
const NERVFS_HOST = process.env.NERVFS_HOST || '127.0.0.1';
const NERVFS_PORT = Number(process.env.NERVFS_PORT || 9000);
const ADMIN_PASSWORD = process.env.ADMIN_PASSWORD;
const AUTH_SECRET = process.env.AUTH_SECRET;
const ALLOWED_ORIGIN = process.env.ALLOWED_ORIGIN || '*';
const TOKEN_TTL_MS = Number(process.env.TOKEN_TTL_MS || 12 * 60 * 60 * 1000);

// Refuse to boot if critical secrets are not set
if (!ADMIN_PASSWORD || !AUTH_SECRET) {
  console.error('[CRITICAL] ADMIN_PASSWORD and AUTH_SECRET environment variables must be set!');
  process.exit(1);
}

// Middleware & CORS Configuration
app.use(cors({
  origin: ALLOWED_ORIGIN, 
  methods: ['GET', 'POST', 'DELETE', 'PATCH'],
  allowedHeaders: ['Content-Type', 'Authorization', 'ngrok-skip-browser-warning'],
}));

app.use(express.json());

// Rate Limiting 
const loginLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  max: 15, 
  message: { error: 'Too many failed login attempts. Try again in 15 minutes.' },
  standardHeaders: true,
  legacyHeaders: false,
});

// Crypto & Token Utilities
function safeCompare(a, b) {
  const bufA = Buffer.from(String(a));
  const bufB = Buffer.from(String(b));
  if (bufA.length !== bufB.length) {
    crypto.timingSafeEqual(bufA, bufA); 
    return false;
  }
  return crypto.timingSafeEqual(bufA, bufB);
}

function signPayload(payload) {
  return crypto.createHmac('sha256', AUTH_SECRET).update(payload).digest('base64url');
}

function createToken() {
  const payload = Buffer.from(
    JSON.stringify({ role: 'admin', exp: Date.now() + TOKEN_TTL_MS }),
  ).toString('base64url');
  return `${payload}.${signPayload(payload)}`;
}

function verifyToken(token) {
  if (!token || !token.includes('.')) {
    return false;
  }

  const [payload, signature] = token.split('.');
  const expected = signPayload(payload);

  if (!safeCompare(signature, expected)) {
    return false;
  }

  try {
    const data = JSON.parse(Buffer.from(payload, 'base64url').toString('utf8'));
    return data.role === 'admin' && Date.now() < data.exp;
  } catch {
    return false;
  }
}

function requireAdmin(req, res, next) {
  const header = req.get('authorization') || '';
  const token = header.startsWith('Bearer ') ? header.slice(7) : String(req.query.token || '');

  if (!verifyToken(token)) {
    return res.status(401).json({ error: 'Admin login required' });
  }

  return next();
}

function statusForError(err) {
  if ([400, 403, 404, 409, 500].includes(err.code)) {
    return err.code;
  }
  return 502;
}

function sendError(res, err) {
  res.status(statusForError(err)).json({ error: err.message });
}

// API Routes
app.get('/health', async (req, res) => {
  try {
    await listFiles(NERVFS_HOST, NERVFS_PORT);
    res.json({ status: 'ok' });
  } catch (err) {
    sendError(res, err);
  }
});

// Applied rate limiting & constant-time password check
app.post('/auth/login', loginLimiter, (req, res) => {
  const password = String(req.body.password || '');

  if (!safeCompare(password, ADMIN_PASSWORD)) {
    return res.status(401).json({ error: 'Invalid admin password' });
  }

  return res.json({ token: createToken() });
});

app.use('/files', requireAdmin);

app.get('/files', async (req, res) => {
  try {
    const { files, serverMs } = await listFiles(NERVFS_HOST, NERVFS_PORT);
    res.json({ files, serverMs });
  } catch (err) {
    sendError(res, err);
  }
});

app.post('/files', upload.single('file'), async (req, res) => {
  try {
    if (!req.file) {
      return res.status(400).json({ error: 'multipart field "file" is required' });
    }

    const { serverMs, metrics } = await uploadFile(
      NERVFS_HOST,
      NERVFS_PORT,
      req.file.originalname,
      req.file.buffer,
    );
    return res.status(201).json({ status: 'ok', serverMs, metrics });
  } catch (err) {
    return sendError(res, err);
  }
});

app.get('/files/:name', async (req, res) => {
  try {
    const filename = req.params.name;
    const { data, serverMs } = await downloadFile(NERVFS_HOST, NERVFS_PORT, filename);

    // Sanitized header output against CRLF / Header Injection
    const safeFilename = encodeURIComponent(filename).replace(/['()]/g, escape).replace(/\*/g, '%2A');
    res.setHeader('Content-Disposition', `attachment; filename="${safeFilename}"; filename*=UTF-8''${safeFilename}`);
    res.setHeader('Content-Type', 'application/octet-stream');
    res.setHeader('X-Server-Time-Ms', String(serverMs));
    res.send(data);
  } catch (err) {
    sendError(res, err);
  }
});

app.delete('/files/:name', async (req, res) => {
  try {
    const { serverMs } = await deleteFile(NERVFS_HOST, NERVFS_PORT, req.params.name);
    res.json({ status: 'ok', serverMs });
  } catch (err) {
    sendError(res, err);
  }
});

app.patch('/files/:name/permissions', async (req, res) => {
  try {
    const mode = Number.parseInt(req.body.mode, 10);
    if (Number.isNaN(mode) || mode < 0 || mode > 0o7777) {
      return res.status(400).json({ error: 'mode must be a numeric permission value' });
    }

    const { serverMs } = await chmodFile(NERVFS_HOST, NERVFS_PORT, req.params.name, mode);
    return res.json({ status: 'ok', serverMs });
  } catch (err) {
    return sendError(res, err);
  }
});

app.listen(BRIDGE_PORT, async () => {
  console.log(`NERV-FS secure bridge listening on :${BRIDGE_PORT}, proxying ${NERVFS_HOST}:${NERVFS_PORT}`);

  if (process.env.NGROK_AUTHTOKEN) {
    try {
      const listener = await ngrok.forward({
        addr: BRIDGE_PORT,
        authtoken: process.env.NGROK_AUTHTOKEN,
        domain: process.env.NGROK_DOMAIN, 
      });
      console.log(`[NGROK] Public tunnel active at: ${listener.url()}`);
    } catch (err) {
      console.error('[NGROK] Failed to start tunnel:', err.message);
    }
  }
});