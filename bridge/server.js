'use strict';

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

app.get('/files', async (req, res) => {
  try {
    const files = await listFiles(NERVFS_HOST, NERVFS_PORT);
    res.json({ files });
  } catch (err) {
    sendError(res, err);
  }
});

app.post('/files', upload.single('file'), async (req, res) => {
  try {
    if (!req.file) {
      return res.status(400).json({ error: 'multipart field "file" is required' });
    }

    await uploadFile(NERVFS_HOST, NERVFS_PORT, req.file.originalname, req.file.buffer);
    return res.status(201).json({ status: 'ok' });
  } catch (err) {
    return sendError(res, err);
  }
});

app.get('/files/:name', async (req, res) => {
  try {
    const data = await downloadFile(NERVFS_HOST, NERVFS_PORT, req.params.name);
    res.setHeader('Content-Disposition', `attachment; filename="${req.params.name}"`);
    res.setHeader('Content-Type', 'application/octet-stream');
    res.send(data);
  } catch (err) {
    sendError(res, err);
  }
});

app.delete('/files/:name', async (req, res) => {
  try {
    await deleteFile(NERVFS_HOST, NERVFS_PORT, req.params.name);
    res.json({ status: 'ok' });
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

    await chmodFile(NERVFS_HOST, NERVFS_PORT, req.params.name, mode);
    return res.json({ status: 'ok' });
  } catch (err) {
    return sendError(res, err);
  }
});

app.listen(BRIDGE_PORT, () => {
  console.log(`NERV-FS bridge listening on :${BRIDGE_PORT}, proxying ${NERVFS_HOST}:${NERVFS_PORT}`);
});
