'use strict';

const net = require('net');

const MAGIC = 0xae;
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
  return buf;
}

function parseHeader(buf) {
  if (buf.length < HEADER_SIZE) {
    throw new Error('response header truncated');
  }

  const header = {
    magic: buf.readUInt8(0),
    version: buf.readUInt8(1),
    opcode: buf.readUInt16BE(2),
    payloadLen: buf.readUInt32BE(4),
  };

  if (header.magic !== MAGIC || header.version !== VERSION) {
    throw new Error('invalid NERV-FS response header');
  }

  return header;
}

function connect(host, port) {
  const socket = net.createConnection({ host, port });

  return new Promise((resolve, reject) => {
    socket.once('connect', () => resolve(socket));
    socket.once('error', reject);
  });
}

function readResponse(socket) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;
    let expected = HEADER_SIZE;
    let header = null;

    function cleanup() {
      socket.off('data', onData);
      socket.off('error', onError);
      socket.off('end', onEnd);
      socket.off('close', onClose);
    }

    function fail(err) {
      cleanup();
      reject(err);
    }

    function tryResolve() {
      if (total < expected) {
        return;
      }

      const data = Buffer.concat(chunks, total);
      if (header === null) {
        header = parseHeader(data.subarray(0, HEADER_SIZE));
        expected = HEADER_SIZE + header.payloadLen;
        if (total < expected) {
          return;
        }
      }

      cleanup();
      resolve({
        header,
        body: data.subarray(HEADER_SIZE, expected),
      });
    }

    function onData(chunk) {
      chunks.push(chunk);
      total += chunk.length;
      try {
        tryResolve();
      } catch (err) {
        fail(err);
      }
    }

    function onError(err) {
      fail(err);
    }

    function onEnd() {
      if (total < expected) {
        fail(new Error('connection closed before full response arrived'));
      }
    }

    function onClose() {
      if (total < expected) {
        fail(new Error('connection closed before full response arrived'));
      }
    }

    socket.on('data', onData);
    socket.on('error', onError);
    socket.on('end', onEnd);
    socket.on('close', onClose);
  });
}

async function request(host, port, opcode, payload = Buffer.alloc(0)) {
  const socket = await connect(host, port);

  try {
    socket.write(Buffer.concat([buildHeader(opcode, payload.length), payload]));
    const response = await readResponse(socket);
    socket.end();
    return response;
  } catch (err) {
    socket.destroy();
    throw err;
  }
}

function throwIfError(header, body) {
  if (header.opcode !== OPCODES.RESP_ERR) {
    return;
  }

  const errCode = body.length >= 2 ? body.readUInt16BE(0) : 500;
  const msgLen = body.length >= 4 ? body.readUInt16BE(2) : 0;
  const msg = body.subarray(4, 4 + msgLen).toString('utf8') || 'NERV-FS request failed';
  const err = new Error(msg);
  err.code = errCode;
  throw err;
}

function expectOpcode(header, expectedOpcode) {
  if (header.opcode !== expectedOpcode) {
    const err = new Error(`unexpected NERV-FS response opcode 0x${header.opcode.toString(16)}`);
    err.code = 502;
    throw err;
  }
}

function filenamePayload(filename, extraBytes = 0) {
  const nameBuf = Buffer.from(filename, 'utf8');
  const payload = Buffer.alloc(2 + nameBuf.length + extraBytes);
  payload.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(payload, 2);
  return { payload, nameEnd: 2 + nameBuf.length };
}

async function listFiles(host, port) {
  const { header, body } = await request(host, port, OPCODES.REQ_LIST);
  throwIfError(header, body);
  expectOpcode(header, OPCODES.RESP_LIST_DATA);

  const count = body.readUInt32BE(0);
  let offset = 4;
  const files = [];

  for (let i = 0; i < count; i += 1) {
    const nameLen = body.readUInt16BE(offset);
    offset += 2;
    const name = body.subarray(offset, offset + nameLen).toString('utf8');
    offset += nameLen;
    const size = body.readBigUInt64BE(offset);
    offset += 8;
    const mode = body.readUInt32BE(offset);
    offset += 4;
    files.push({ name, size: size.toString(), mode });
  }

  return files;
}

async function uploadFile(host, port, filename, fileBuffer) {
  const nameBuf = Buffer.from(filename, 'utf8');
  const meta = Buffer.alloc(2 + nameBuf.length + 8);

  meta.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(meta, 2);
  meta.writeBigUInt64BE(BigInt(fileBuffer.length), 2 + nameBuf.length);

  const payload = Buffer.concat([meta, fileBuffer]);
  const { header, body } = await request(host, port, OPCODES.REQ_UPLOAD, payload);
  throwIfError(header, body);
  expectOpcode(header, OPCODES.RESP_OK);
}

async function downloadFile(host, port, filename) {
  const { payload } = filenamePayload(filename);
  const { header, body } = await request(host, port, OPCODES.REQ_DOWNLOAD, payload);
  throwIfError(header, body);
  expectOpcode(header, OPCODES.RESP_FILE_DATA);

  const fileSize = Number(body.readBigUInt64BE(0));
  return body.subarray(8, 8 + fileSize);
}

async function deleteFile(host, port, filename) {
  const { payload } = filenamePayload(filename);
  const { header, body } = await request(host, port, OPCODES.REQ_DELETE, payload);
  throwIfError(header, body);
  expectOpcode(header, OPCODES.RESP_OK);
}

async function chmodFile(host, port, filename, mode) {
  const { payload, nameEnd } = filenamePayload(filename, 4);
  payload.writeUInt32BE(mode, nameEnd);

  const { header, body } = await request(host, port, OPCODES.REQ_CHMOD, payload);
  throwIfError(header, body);
  expectOpcode(header, OPCODES.RESP_OK);
}

module.exports = {
  listFiles,
  uploadFile,
  downloadFile,
  deleteFile,
  chmodFile,
  OPCODES,
};
