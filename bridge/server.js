'use strict';

const crypto = require('crypto');
const express = require('express');
const multer = require('multer');
const cors = require('cors');
const {
  listFiles,
  uploadFile,
  downloadFile,
  deleteFile,
  chmodFile,
} = require('./nervfsClient');

const app = express();
const upload = multer();

app.use(cors());
app.use(express.json());

const BRIDGE_PORT = Number(process.env.BRIDGE_PORT || 4000);
const NERVFS_HOST = process.env.NERVFS_HOST || '127.0.0.1';
const NERVFS_PORT = Number(process.env.NERVFS_PORT || 9000);
const ADMIN_PASSWORD = process.env.ADMIN_PASSWORD || 'admin';
const AUTH_SECRET = process.env.AUTH_SECRET || ADMIN_PASSWORD;
const TOKEN_TTL_MS = Number(process.env.TOKEN_TTL_MS || 12 * 60 * 60 * 1000);

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
  if (
    signature.length !== expected.length ||
    !crypto.timingSafeEqual(Buffer.from(signature), Buffer.from(expected))
  ) {
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

app.get('/health', async (req, res) => {
  try {
    await listFiles(NERVFS_HOST, NERVFS_PORT);
    res.json({ status: 'ok' });
  } catch (err) {
    sendError(res, err);
  }
});

app.post('/auth/login', (req, res) => {
  const password = String(req.body.password || '');

  if (password !== ADMIN_PASSWORD) {
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
    const { data, serverMs } = await downloadFile(NERVFS_HOST, NERVFS_PORT, req.params.name);
    res.setHeader('Content-Disposition', `attachment; filename="${req.params.name}"`);
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

app.listen(BRIDGE_PORT, () => {
  console.log(`NERV-FS bridge listening on :${BRIDGE_PORT}, proxying ${NERVFS_HOST}:${NERVFS_PORT}`);
});