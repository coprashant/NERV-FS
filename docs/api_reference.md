# NERV-FS wire protocol reference

This is everything needed to build the Node bridge and the React UI against
the C server without reading its source. The server speaks a small custom
binary protocol over raw TCP, not HTTP, so the bridge's job is to translate
REST calls into this protocol and back.

## 1. Connection model

- The server listens on one TCP port, given as a command line argument when
  it starts, for example `./nervfs_server 9000`.
- There is no handshake and no authentication.
- Every request is its own fresh TCP connection: connect, send one request,
  read one response, close the socket. The server does not keep a
  connection open across multiple requests, so the bridge should open a new
  socket per incoming REST call.
- Storage is a single flat directory, there are no subfolders. Filenames
  cannot contain a slash.

## 2. The 16 byte header

Every message, request or response, starts with this fixed 16 byte header.
All multi byte fields are big endian, also called network byte order.

| offset | size | field       | notes                                   |
|--------|------|-------------|------------------------------------------|
| 0      | 1    | magic       | always `0xAE`                             |
| 1      | 1    | version     | always `1`                                |
| 2      | 2    | opcode      | uint16, see table below                   |
| 4      | 4    | payload_len | uint32, byte length of the payload that follows |
| 8      | 8    | reserved    | uint64, always zero, ignore on read       |

If magic or version do not match exactly the server closes the connection
without responding, so always send both correctly.

## 3. Opcodes

Requests, sent by the client:

| name          | value  |
|---------------|--------|
| REQ_UPLOAD    | 0x01   |
| REQ_DOWNLOAD  | 0x02   |
| REQ_LIST      | 0x03   |
| REQ_DELETE    | 0x04   |
| REQ_CHMOD     | 0x05   |

Responses, sent by the server:

| name             | value  |
|------------------|--------|
| RESP_OK          | 0x80   |
| RESP_ERR         | 0x81   |
| RESP_FILE_DATA   | 0x82   |
| RESP_LIST_DATA   | 0x83   |

## 4. Error codes

These show up inside a RESP_ERR payload, see format below.

| code | meaning                                  |
|------|-------------------------------------------|
| 400  | bad request, malformed payload             |
| 403  | forbidden, filename failed validation       |
| 404  | not found                                  |
| 409  | already exists, currently unused, upload always overwrites |
| 500  | internal error on the server                |

## 5. Payload formats

### REQ_UPLOAD

```
[2B name_len][name bytes][8B file_size][file bytes]
```

`name_len` and `file_size` must exactly match the bytes that follow, the
server rejects the request otherwise. Response is RESP_OK with an empty
payload on success, or RESP_ERR.

### REQ_DOWNLOAD

```
[2B name_len][name bytes]
```

Response on success is RESP_FILE_DATA with payload:

```
[8B file_size][file bytes]
```

Response on failure is RESP_ERR, most commonly 404 if the name does not
exist.

### REQ_LIST

No payload, payload_len is 0.

Response is RESP_LIST_DATA with payload:

```
[4B count]
  repeated count times:
  [2B name_len][name bytes][8B size][4B mode]
```

`mode` is the raw permission bits, for example 420 decimal is 0644 octal.

### REQ_DELETE

```
[2B name_len][name bytes]
```

Response is RESP_OK or RESP_ERR, 404 if the name does not exist.

### REQ_CHMOD

```
[2B name_len][name bytes][4B mode]
```

`mode` should be the numeric permission value, for example 0644 which is
420 decimal, not a string. Response is RESP_OK or RESP_ERR.

### RESP_ERR

Every error response, regardless of which request caused it, has this
payload shape:

```
[2B err_code][2B msg_len][msg bytes]
```

`msg` is a short human readable string, useful to show directly in the UI.

## 6. Filename rules

The server validates every filename before touching disk and rejects with
403 forbidden otherwise:

- maximum 255 bytes
- no `/` anywhere in the name, storage is flat, there are no subfolders
- cannot be exactly `.` or `..`
- no embedded null bytes

Worth mirroring these same checks client side in the React UI so the user
gets an immediate inline error instead of waiting on a round trip.

## 7. Concurrency and locking, useful context for the bridge

- The server has an internal thread pool, it is safe to have several
  requests in flight at once, each as its own TCP connection.
- Uploads and deletes to the same filename are serialized against each
  other, one will wait for the other to finish rather than corrupting the
  file.
- A download of a file currently being uploaded will wait until the upload
  finishes, then read the complete new file, it never reads a half written
  file.
- None of this needs any special handling from the bridge, it is purely a
  server side guarantee. Just be aware that two requests to the same
  filename might take a little longer than usual since one is waiting on
  the other.

## 8. Suggested REST mapping

Not required, just a natural shape for the bridge's own routes:

| REST                          | maps to                     |
|-------------------------------|------------------------------|
| GET /files                    | REQ_LIST                     |
| POST /files (multipart)       | REQ_UPLOAD                   |
| GET /files/:name              | REQ_DOWNLOAD                 |
| DELETE /files/:name           | REQ_DELETE                    |
| PATCH /files/:name/mode       | REQ_CHMOD                     |

## 9. Worked example in Node

This is a full working request and response cycle for REQ_LIST, using only
the built in `net` module. Every other opcode follows the exact same shape,
only the payload changes.

```javascript
const net = require('net');

const MAGIC = 0xAE;
const VERSION = 1;
const HEADER_SIZE = 16;

const OPCODES = {
  REQ_UPLOAD: 0x01,
  REQ_DOWNLOAD: 0x02,
  REQ_LIST: 0x03,
  REQ_DELETE: 0x04,
  REQ_CHMOD: 0x05,
  RESP_OK: 0x80,
  RESP_ERR: 0x81,
  RESP_FILE_DATA: 0x82,
  RESP_LIST_DATA: 0x83,
};

function buildHeader(opcode, payloadLen) {
  const buf = Buffer.alloc(HEADER_SIZE);
  buf.writeUInt8(MAGIC, 0);
  buf.writeUInt8(VERSION, 1);
  buf.writeUInt16BE(opcode, 2);
  buf.writeUInt32BE(payloadLen, 4);
  // bytes 8 through 15 are the reserved field, already zero from alloc
  return buf;
}

function parseHeader(buf) {
  return {
    magic: buf.readUInt8(0),
    version: buf.readUInt8(1),
    opcode: buf.readUInt16BE(2),
    payloadLen: buf.readUInt32BE(4),
  };
}

// reads exactly n bytes from a socket, resolves a Buffer, since a single
// data event is not guaranteed to contain the full message
function readExact(socket, n) {
  return new Promise((resolve, reject) => {
    let chunks = [];
    let total = 0;

    function onData(chunk) {
      chunks.push(chunk);
      total += chunk.length;
      if (total >= n) {
        cleanup();
        const combined = Buffer.concat(chunks, total);
        resolve(combined.subarray(0, n));
        // note, any extra bytes past n are dropped here, fine as long as
        // you always call readExact for exactly the next expected piece
      }
    }

    function onError(err) {
      cleanup();
      reject(err);
    }

    function onEnd() {
      cleanup();
      reject(new Error('connection closed before enough bytes arrived'));
    }

    function cleanup() {
      socket.off('data', onData);
      socket.off('error', onError);
      socket.off('end', onEnd);
    }

    socket.on('data', onData);
    socket.on('error', onError);
    socket.on('end', onEnd);
  });
}

async function requestList(host, port) {
  const socket = net.createConnection({ host, port });

  await new Promise((resolve, reject) => {
    socket.once('connect', resolve);
    socket.once('error', reject);
  });

  socket.write(buildHeader(OPCODES.REQ_LIST, 0));

  const headerBuf = await readExact(socket, HEADER_SIZE);
  const header = parseHeader(headerBuf);

  if (header.opcode === OPCODES.RESP_ERR) {
    const body = await readExact(socket, header.payloadLen);
    const errCode = body.readUInt16BE(0);
    const msgLen = body.readUInt16BE(2);
    const msg = body.subarray(4, 4 + msgLen).toString('utf8');
    socket.end();
    throw new Error(`server error ${errCode}: ${msg}`);
  }

  const body = await readExact(socket, header.payloadLen);
  const count = body.readUInt32BE(0);
  let offset = 4;
  const files = [];

  for (let i = 0; i < count; i++) {
    const nameLen = body.readUInt16BE(offset);
    offset += 2;
    const name = body.subarray(offset, offset + nameLen).toString('utf8');
    offset += nameLen;
    const size = body.readBigUInt64BE(offset);
    offset += 8;
    const mode = body.readUInt32BE(offset);
    offset += 4;
    files.push({ name, size, mode });
  }

  socket.end();
  return files;
}

// requestList('127.0.0.1', 9000).then(console.log).catch(console.error);
```

For REQ_UPLOAD, build the payload the same way, `name_len` then name bytes
then `file_size` as an 8 byte big endian value, `Buffer.writeBigUInt64BE`
works well for that part since file sizes can exceed 32 bits, then the raw
file bytes, then send the header with the correct payload_len followed by
that payload buffer.

## 10. Quick reference, testing without writing any bridge code yet

The C project already ships a small CLI test client that speaks this exact
protocol, useful for sanity checking the server directly while the bridge
is being built:

```
./nervfs_client 127.0.0.1 9000 list
./nervfs_client 127.0.0.1 9000 upload local_file.txt remote_name.txt
./nervfs_client 127.0.0.1 9000 download remote_name.txt local_copy.txt
./nervfs_client 127.0.0.1 9000 delete remote_name.txt
./nervfs_client 127.0.0.1 9000 chmod remote_name.txt 644
```

## 11. Worked examples for the remaining opcodes

The scaffolding from section 9 (the constants, `buildHeader`, `parseHeader`,
and `readExact`) is shared by all operations. The examples below assume that
code is already present — only the operation-specific part is shown.

### REQ_UPLOAD

```javascript
async function requestUpload(host, port, filename, fileBuffer) {
  const socket = net.createConnection({ host, port });
  await new Promise((resolve, reject) => {
    socket.once('connect', resolve);
    socket.once('error', reject);
  });

  const nameBuf = Buffer.from(filename, 'utf8');

  // payload: [2B name_len][name bytes][8B file_size][file bytes]
  const meta = Buffer.alloc(2 + nameBuf.length + 8);
  meta.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(meta, 2);
  meta.writeBigUInt64BE(BigInt(fileBuffer.length), 2 + nameBuf.length);

  const payload = Buffer.concat([meta, fileBuffer]);
  socket.write(Buffer.concat([buildHeader(OPCODES.REQ_UPLOAD, payload.length), payload]));

  const headerBuf = await readExact(socket, HEADER_SIZE);
  const header = parseHeader(headerBuf);

  if (header.opcode === OPCODES.RESP_ERR) {
    const body = await readExact(socket, header.payloadLen);
    const errCode = body.readUInt16BE(0);
    const msgLen = body.readUInt16BE(2);
    const msg = body.subarray(4, 4 + msgLen).toString('utf8');
    socket.end();
    throw new Error(`server error ${errCode}: ${msg}`);
  }

  // RESP_OK with empty payload — nothing to read
  socket.end();
}
```

### REQ_DOWNLOAD

```javascript
async function requestDownload(host, port, filename) {
  const socket = net.createConnection({ host, port });
  await new Promise((resolve, reject) => {
    socket.once('connect', resolve);
    socket.once('error', reject);
  });

  const nameBuf = Buffer.from(filename, 'utf8');

  // payload: [2B name_len][name bytes]
  const payload = Buffer.alloc(2 + nameBuf.length);
  payload.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(payload, 2);

  socket.write(Buffer.concat([buildHeader(OPCODES.REQ_DOWNLOAD, payload.length), payload]));

  const headerBuf = await readExact(socket, HEADER_SIZE);
  const header = parseHeader(headerBuf);

  if (header.opcode === OPCODES.RESP_ERR) {
    const body = await readExact(socket, header.payloadLen);
    const errCode = body.readUInt16BE(0);
    const msgLen = body.readUInt16BE(2);
    const msg = body.subarray(4, 4 + msgLen).toString('utf8');
    socket.end();
    throw new Error(`server error ${errCode}: ${msg}`);
  }

  // RESP_FILE_DATA payload: [8B file_size][file bytes]
  const body = await readExact(socket, header.payloadLen);
  const fileSize = body.readBigUInt64BE(0);
  const fileData = body.subarray(8, 8 + Number(fileSize));

  socket.end();
  return fileData; // Buffer containing the raw file bytes
}
```

### REQ_DELETE

```javascript
async function requestDelete(host, port, filename) {
  const socket = net.createConnection({ host, port });
  await new Promise((resolve, reject) => {
    socket.once('connect', resolve);
    socket.once('error', reject);
  });

  const nameBuf = Buffer.from(filename, 'utf8');

  // payload: [2B name_len][name bytes]
  const payload = Buffer.alloc(2 + nameBuf.length);
  payload.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(payload, 2);

  socket.write(Buffer.concat([buildHeader(OPCODES.REQ_DELETE, payload.length), payload]));

  const headerBuf = await readExact(socket, HEADER_SIZE);
  const header = parseHeader(headerBuf);

  if (header.opcode === OPCODES.RESP_ERR) {
    const body = await readExact(socket, header.payloadLen);
    const errCode = body.readUInt16BE(0);
    const msgLen = body.readUInt16BE(2);
    const msg = body.subarray(4, 4 + msgLen).toString('utf8');
    socket.end();
    throw new Error(`server error ${errCode}: ${msg}`);
  }

  socket.end();
}
```

### REQ_CHMOD

```javascript
async function requestChmod(host, port, filename, mode) {
  const socket = net.createConnection({ host, port });
  await new Promise((resolve, reject) => {
    socket.once('connect', resolve);
    socket.once('error', reject);
  });

  const nameBuf = Buffer.from(filename, 'utf8');

  // payload: [2B name_len][name bytes][4B mode]
  // mode is a numeric value, for example 0o644 === 420 decimal
  const payload = Buffer.alloc(2 + nameBuf.length + 4);
  payload.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(payload, 2);
  payload.writeUInt32BE(mode, 2 + nameBuf.length);

  socket.write(Buffer.concat([buildHeader(OPCODES.REQ_CHMOD, payload.length), payload]));

  const headerBuf = await readExact(socket, HEADER_SIZE);
  const header = parseHeader(headerBuf);

  if (header.opcode === OPCODES.RESP_ERR) {
    const body = await readExact(socket, header.payloadLen);
    const errCode = body.readUInt16BE(0);
    const msgLen = body.readUInt16BE(2);
    const msg = body.subarray(4, 4 + msgLen).toString('utf8');
    socket.end();
    throw new Error(`server error ${errCode}: ${msg}`);
  }

  socket.end();
}
```

---

## 12. Bridge implementation (`bridge/`)

### 12.1 Dependencies

```bash
cd bridge && npm install express cors multer
```

- **express** — HTTP server and router
- **cors** — lets the Vite dev server on port 5173 call the bridge on port 4000
- **multer** — parses `multipart/form-data` uploads and gives you the file as a Buffer in memory

### 12.2 `bridge/nervfsClient.js`

Full file. Export the five operation functions and the raw `OPCODES` map so
`server.js` can use the constants for any manual header inspection if needed.

```javascript
'use strict';
const net = require('net');

const MAGIC = 0xAE;
const VERSION = 1;
const HEADER_SIZE = 16;

const OPCODES = {
  REQ_UPLOAD:    0x01,
  REQ_DOWNLOAD:  0x02,
  REQ_LIST:      0x03,
  REQ_DELETE:    0x04,
  REQ_CHMOD:     0x05,
  RESP_OK:       0x80,
  RESP_ERR:      0x81,
  RESP_FILE_DATA: 0x82,
  RESP_LIST_DATA: 0x83,
};

function buildHeader(opcode, payloadLen) {
  const buf = Buffer.alloc(HEADER_SIZE);
  buf.writeUInt8(MAGIC, 0);
  buf.writeUInt8(VERSION, 1);
  buf.writeUInt16BE(opcode, 2);
  buf.writeUInt32BE(payloadLen, 4);
  // bytes 8-15 are reserved, stay zero
  return buf;
}

function parseHeader(buf) {
  return {
    magic:      buf.readUInt8(0),
    version:    buf.readUInt8(1),
    opcode:     buf.readUInt16BE(2),
    payloadLen: buf.readUInt32BE(4),
  };
}

function readExact(socket, n) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;

    function onData(chunk) {
      chunks.push(chunk);
      total += chunk.length;
      if (total >= n) {
        cleanup();
        resolve(Buffer.concat(chunks, total).subarray(0, n));
      }
    }
    function onError(err) { cleanup(); reject(err); }
    function onEnd()  { cleanup(); reject(new Error('connection closed early')); }
    function cleanup() {
      socket.off('data', onData);
      socket.off('error', onError);
      socket.off('end', onEnd);
    }

    socket.on('data', onData);
    socket.on('error', onError);
    socket.on('end', onEnd);
  });
}

function connect(host, port) {
  const socket = net.createConnection({ host, port });
  return new Promise((resolve, reject) => {
    socket.once('connect', () => resolve(socket));
    socket.once('error', reject);
  });
}

async function checkError(socket, header) {
  if (header.opcode === OPCODES.RESP_ERR) {
    const body = await readExact(socket, header.payloadLen);
    const errCode = body.readUInt16BE(0);
    const msgLen  = body.readUInt16BE(2);
    const msg     = body.subarray(4, 4 + msgLen).toString('utf8');
    socket.end();
    const err = new Error(msg);
    err.code = errCode;
    throw err;
  }
}

async function listFiles(host, port) {
  const socket = await connect(host, port);
  socket.write(buildHeader(OPCODES.REQ_LIST, 0));

  const header = parseHeader(await readExact(socket, HEADER_SIZE));
  await checkError(socket, header);

  const body  = await readExact(socket, header.payloadLen);
  const count = body.readUInt32BE(0);
  let offset  = 4;
  const files = [];

  for (let i = 0; i < count; i++) {
    const nameLen = body.readUInt16BE(offset); offset += 2;
    const name    = body.subarray(offset, offset + nameLen).toString('utf8'); offset += nameLen;
    const size    = body.readBigUInt64BE(offset); offset += 8;
    const mode    = body.readUInt32BE(offset);    offset += 4;
    files.push({ name, size: size.toString(), mode });
  }

  socket.end();
  return files;
}

async function uploadFile(host, port, filename, fileBuffer) {
  const socket  = await connect(host, port);
  const nameBuf = Buffer.from(filename, 'utf8');
  const meta    = Buffer.alloc(2 + nameBuf.length + 8);

  meta.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(meta, 2);
  meta.writeBigUInt64BE(BigInt(fileBuffer.length), 2 + nameBuf.length);

  const payload = Buffer.concat([meta, fileBuffer]);
  socket.write(Buffer.concat([buildHeader(OPCODES.REQ_UPLOAD, payload.length), payload]));

  const header = parseHeader(await readExact(socket, HEADER_SIZE));
  await checkError(socket, header);
  socket.end();
}

async function downloadFile(host, port, filename) {
  const socket  = await connect(host, port);
  const nameBuf = Buffer.from(filename, 'utf8');
  const payload = Buffer.alloc(2 + nameBuf.length);

  payload.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(payload, 2);
  socket.write(Buffer.concat([buildHeader(OPCODES.REQ_DOWNLOAD, payload.length), payload]));

  const header = parseHeader(await readExact(socket, HEADER_SIZE));
  await checkError(socket, header);

  const body     = await readExact(socket, header.payloadLen);
  const fileSize = Number(body.readBigUInt64BE(0));
  const fileData = body.subarray(8, 8 + fileSize);

  socket.end();
  return fileData;
}

async function deleteFile(host, port, filename) {
  const socket  = await connect(host, port);
  const nameBuf = Buffer.from(filename, 'utf8');
  const payload = Buffer.alloc(2 + nameBuf.length);

  payload.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(payload, 2);
  socket.write(Buffer.concat([buildHeader(OPCODES.REQ_DELETE, payload.length), payload]));

  const header = parseHeader(await readExact(socket, HEADER_SIZE));
  await checkError(socket, header);
  socket.end();
}

async function chmodFile(host, port, filename, mode) {
  const socket  = await connect(host, port);
  const nameBuf = Buffer.from(filename, 'utf8');
  const payload = Buffer.alloc(2 + nameBuf.length + 4);

  payload.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(payload, 2);
  payload.writeUInt32BE(mode, 2 + nameBuf.length);
  socket.write(Buffer.concat([buildHeader(OPCODES.REQ_CHMOD, payload.length), payload]));

  const header = parseHeader(await readExact(socket, HEADER_SIZE));
  await checkError(socket, header);
  socket.end();
}

module.exports = { listFiles, uploadFile, downloadFile, deleteFile, chmodFile, OPCODES };
```

Note on `size` serialization: `readBigUInt64BE` returns a `BigInt`. JSON
serialization will throw if you pass a raw BigInt, so the `listFiles`
function above calls `.toString()` on it. The React UI should treat the
size field as a string and format it for display (e.g. `Number(size)` is
safe for files under 2^53 bytes, which covers any realistic use).

### 12.3 `bridge/server.js`

Full file. The bridge listens on port 4000. Adjust `NERVFS_HOST` and
`NERVFS_PORT` if you run the C server on a different address.

```javascript
'use strict';
const express = require('express');
const multer  = require('multer');
const cors    = require('cors');
const { listFiles, uploadFile, downloadFile, deleteFile, chmodFile } = require('./nervfsClient');

const app    = express();
const upload = multer(); // keeps uploaded files as Buffers in memory

app.use(cors());
app.use(express.json());

const NERVFS_HOST = '127.0.0.1';
const NERVFS_PORT = 9000;

// GET /files  →  REQ_LIST
app.get('/files', async (req, res) => {
  try {
    const files = await listFiles(NERVFS_HOST, NERVFS_PORT);
    res.json({ files });
  } catch (err) {
    res.status(err.code === 500 ? 500 : 502).json({ error: err.message });
  }
});

// POST /files  →  REQ_UPLOAD
// expects multipart/form-data with a single field named "file"
app.post('/files', upload.single('file'), async (req, res) => {
  try {
    await uploadFile(NERVFS_HOST, NERVFS_PORT, req.file.originalname, req.file.buffer);
    res.status(201).json({ status: 'ok' });
  } catch (err) {
    const status = err.code === 403 ? 403 : err.code === 409 ? 409 : 502;
    res.status(status).json({ error: err.message });
  }
});

// GET /files/:name  →  REQ_DOWNLOAD
// streams the file back as an attachment so the browser triggers a download
app.get('/files/:name', async (req, res) => {
  try {
    const data = await downloadFile(NERVFS_HOST, NERVFS_PORT, req.params.name);
    res.setHeader('Content-Disposition', `attachment; filename="${req.params.name}"`);
    res.setHeader('Content-Type', 'application/octet-stream');
    res.send(data);
  } catch (err) {
    const status = err.code === 404 ? 404 : err.code === 403 ? 403 : 502;
    res.status(status).json({ error: err.message });
  }
});

// DELETE /files/:name  →  REQ_DELETE
app.delete('/files/:name', async (req, res) => {
  try {
    await deleteFile(NERVFS_HOST, NERVFS_PORT, req.params.name);
    res.json({ status: 'ok' });
  } catch (err) {
    const status = err.code === 404 ? 404 : err.code === 403 ? 403 : 502;
    res.status(status).json({ error: err.message });
  }
});

// PATCH /files/:name/permissions  →  REQ_CHMOD
// body: { "mode": 420 }   (420 decimal == 0644 octal)
app.patch('/files/:name/permissions', async (req, res) => {
  try {
    const mode = parseInt(req.body.mode, 10);
    if (isNaN(mode) || mode < 0 || mode > 0o7777) {
      return res.status(400).json({ error: 'mode must be a numeric permission value' });
    }
    await chmodFile(NERVFS_HOST, NERVFS_PORT, req.params.name, mode);
    res.json({ status: 'ok' });
  } catch (err) {
    const status = err.code === 404 ? 404 : err.code === 403 ? 403 : 502;
    res.status(status).json({ error: err.message });
  }
});

app.listen(4000, () => console.log('NERV-FS bridge listening on :4000'));
```

---

## 13. React API layer (`frontend/src/api/nervfsApi.js`)

Thin axios wrapper. All fetch logic lives here — no `axios` calls anywhere
else in the component tree.

```javascript
import axios from 'axios';

const BASE = 'http://localhost:4000';

// returns { files: [{ name, size, mode }, ...] }
export const listFiles = () =>
  axios.get(`${BASE}/files`).then(r => r.data);

// file: a browser File object
// onProgress: called with a number 0-100 as bytes are sent to the bridge
export const uploadFile = (file, onProgress) => {
  const form = new FormData();
  form.append('file', file);
  return axios.post(`${BASE}/files`, form, {
    onUploadProgress: (e) => onProgress && onProgress(Math.round((e.loaded * 100) / e.total)),
  });
};

// triggers a browser download by redirecting; no axios involved
export const downloadFile = (name) => {
  window.location.href = `${BASE}/files/${encodeURIComponent(name)}`;
};

export const deleteFile = (name) =>
  axios.delete(`${BASE}/files/${encodeURIComponent(name)}`);

// mode: number, e.g. 0o644 === 420
export const setPermissions = (name, mode) =>
  axios.patch(`${BASE}/files/${encodeURIComponent(name)}/permissions`, { mode });
```

The `onUploadProgress` callback tracks the browser→bridge leg of the
transfer, which is the bulk of the time for large files. The bridge→C-server
leg is loopback traffic and is fast enough not to need separate progress
reporting.

---

## 14. React component guide

| File | Responsibility |
|------|----------------|
| `App.jsx` | Top-level layout. Holds `files` state, fetches the list on mount and after every upload, delete, or chmod. Passes the list down to `FileList` and the refresh callback to `UploadForm`. |
| `FileList.jsx` | Renders one table row per file: name, formatted size, octal permissions, and action buttons (download, delete, open permissions modal). Receives the list as a prop and calls `deleteFile` / `downloadFile` from the API layer directly. |
| `UploadForm.jsx` | `<input type="file">` element. On change, calls `uploadFile` with an `onProgress` callback that updates local `progress` state. On completion, calls the `onUploaded` prop to trigger a list refresh in `App`. |
| `ProgressBar.jsx` | Stateless. Accepts a `value` prop (0–100) and renders a `<progress>` element. Displayed by `UploadForm` while an upload is in flight. |
| `PermissionsModal.jsx` | Small modal (or inline form). Accepts `filename` and `currentMode` props. Renders a numeric text input pre-filled with the octal string of `currentMode` (e.g. `(420).toString(8)` → `"644"`). On submit, parses the input back to decimal and calls `setPermissions`. |

Formatting tips:
- **size** comes from the server as a string (see section 12.2 note). Use
  `Number(size)` to convert it, then a helper like:
  ```javascript
  const fmt = (n) => n >= 1e9 ? `${(n/1e9).toFixed(1)} GB`
                    : n >= 1e6 ? `${(n/1e6).toFixed(1)} MB`
                    : n >= 1e3 ? `${(n/1e3).toFixed(1)} KB`
                    : `${n} B`;
  ```
- **mode** comes as a decimal integer. Display it as octal:
  `(mode).toString(8).padStart(4, '0')` gives `"0644"`.
- **Filename validation** — mirror section 6 rules in the UI so the user
  gets an immediate inline error without a round trip. A simple check:
  ```javascript
  const isValidName = (n) =>
    n.length > 0 && n.length <= 255 &&
    !n.includes('/') && n !== '.' && n !== '..' && !/\0/.test(n);
  ```

---

## 15. Running the full stack

Three processes, three terminals, start them in this order:

```bash
# terminal 1 — C engine
./nervfs_server 9000

# terminal 2 — Node bridge
cd bridge && node server.js

# terminal 3 — React dev server
cd frontend && npm run dev
```

Then open the URL printed by Vite (typically `http://localhost:5173`).

Quick smoke test without the UI:

```bash
# confirm bridge is up and C server is reachable
curl http://localhost:4000/files

# upload a file
curl -F "file=@/etc/hostname" http://localhost:4000/files

# list again, should show the file
curl http://localhost:4000/files

# download it
curl -O http://localhost:4000/files/hostname

# set permissions
curl -X PATCH http://localhost:4000/files/hostname/permissions \
     -H 'Content-Type: application/json' \
     -d '{"mode": 420}'

# delete
curl -X DELETE http://localhost:4000/files/hostname
```