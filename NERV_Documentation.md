# NERV-FS
### Network-Enabled Efficient Remote Virtual File System
**A concurrent, native C, self-hosted network file server for Linux**

---

## 0. How to Use This Document

This is a from-scratch build guide, ordered as a set of **phases**. Each phase is independently testable — don't move to the next one until the current one compiles, runs, and passes its test checklist. That's the only way a systems project like this stays debuggable.

Recommended pace for a semester project: ~1 phase per week (Phases 0–8), leaving the last 2–3 weeks for polish, diagnostics writeups, and demo prep.

---

## 1. System Overview

NERV-FS is a multi-threaded TCP daemon that lets multiple clients upload, download, list, and manage files on a host machine's filesystem, using only POSIX system calls — no HTTP libraries, no web frameworks, no third-party runtime.

**Three engines, one daemon:**

| Engine | Responsibility | Key syscalls/APIs |
|---|---|---|
| Network & Communication | Accept and manage TCP connections | `socket`, `bind`, `listen`, `accept`, `select`/`poll` |
| Concurrency & Thread Management | Distribute client work across a fixed thread pool | `pthread_create`, `pthread_mutex_t`, `pthread_cond_t` |
| File I/O & Memory Optimization | Read/write files with zero-copy where possible | `open`, `read`, `write`, `mmap`, `munmap`, `fcntl` |

Since v1 has **no authentication** (per your call), every connected client can act on the shared file store. Treat the server root as a sandboxed directory — never point it at a real home directory during testing.

---

## 2. Project Structure

```
nerv-fs/
├── Makefile
├── documentation.md
├── include/
│   ├── protocol.h        # opcode + header definitions
│   ├── server.h          # server-wide structs, globals
│   ├── threadpool.h
│   ├── fileops.h
│   └── logging.h
├── src/
│   ├── main.c            # entrypoint, arg parsing, signal setup
│   ├── net.c             # socket, bind, listen, accept, select loop
│   ├── threadpool.c      # worker pool, job queue, mutex/cond
│   ├── protocol.c        # message parsing/encoding
│   ├── fileops.c         # open/read/write/mmap/fcntl wrappers
│   ├── handlers.c        # UPLOAD/DOWNLOAD/LIST/DELETE/CHMOD logic
│   └── logging.c
├── client/
│   └── nervfs_client.c   # minimal CLI client for testing
├── storage/              # server's sandboxed file root (gitignored)
├── tests/
│   ├── test_upload.sh
│   ├── test_concurrent_clients.sh
│   └── stress_test.py
├── bridge/               # Node.js REST↔binary-protocol gateway, see Section 10
│   ├── package.json
│   ├── server.js
│   └── nervfsClient.js   # speaks the Section 4 wire protocol over TCP
├── frontend/             # React web UI, see Section 11
│   ├── package.json
│   └── src/
│       ├── App.jsx
│       ├── api/nervfsApi.js
│       └── components/
│           ├── FileList.jsx
│           ├── UploadForm.jsx
│           ├── ProgressBar.jsx
│           └── PermissionsModal.jsx
└── docs/
    ├── protocol_spec.md   # (optional split-out of section 4 below)
    └── diagnostics.md     # strace/wireshark notes, see Phase 8
```

---

## 3. Build & Environment Setup

**Target OS:** Linux (Ubuntu 22.04+/Debian recommended; WSL2 works but sockets/mmap behavior should be verified natively before demo day).

**Dependencies:**
```bash
sudo apt update
sudo apt install build-essential gdb strace tcpdump wireshark netcat-openbsd
```

**Minimal Makefile to start with:**
```makefile
CC = gcc
CFLAGS = -Wall -Wextra -pthread -g -D_GNU_SOURCE
SRC = src/main.c src/net.c src/threadpool.c src/protocol.c src/fileops.c src/handlers.c src/logging.c
OBJ = $(SRC:.c=.o)
TARGET = nervfs_server

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
```

`-pthread` is mandatory or mutex/thread functions will silently misbehave or fail to link. `-D_GNU_SOURCE` is needed for some POSIX extensions you'll want later (e.g. `pthread_setname_np`, certain `fcntl` flags).

---

## 4. Wire Protocol (Custom Binary)

This is the part that has no "one true way" — you're designing it. Below is a complete, implementable spec. Feel free to adjust field sizes, but keep it **fixed-size header + variable-size payload**, which is the standard pattern for binary protocols and keeps parsing simple.

### 4.1 Message Framing

Every message (both client→server and server→client) starts with a fixed 16-byte header:

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;        // always 0xAE, sanity check for stream sync
    uint8_t  version;      // protocol version, start at 1
    uint16_t opcode;       // operation code, see table below
    uint32_t payload_len;  // length of payload that follows, in bytes
    uint64_t reserved;     // future use (flags, request id) — zero for now
} nervfs_header_t;
#pragma pack(pop)
```

`#pragma pack(push, 1)` prevents the compiler from padding the struct — critical for binary protocols, since padding differs across compilers/architectures and will desync your parser.

After the header, `payload_len` bytes follow as the message body — its shape depends on the opcode (defined below).

### 4.2 Opcodes

| Opcode | Name | Direction | Payload |
|---|---|---|---|
| 0x01 | REQ_UPLOAD | C→S | `[2B name_len][name][8B file_size][file bytes...]` |
| 0x02 | REQ_DOWNLOAD | C→S | `[2B name_len][name]` |
| 0x03 | REQ_LIST | C→S | *(empty)* |
| 0x04 | REQ_DELETE | C→S | `[2B name_len][name]` |
| 0x05 | REQ_CHMOD | C→S | `[2B name_len][name][4B mode]` |
| 0x80 | RESP_OK | S→C | operation-dependent (see below) |
| 0x81 | RESP_ERR | S→C | `[2B err_code][2B msg_len][msg]` |
| 0x82 | RESP_FILE_DATA | S→C | `[8B file_size][file bytes...]` (reply to REQ_DOWNLOAD) |
| 0x83 | RESP_LIST_DATA | S→C | `[4B entry_count][entries...]` each entry: `[2B name_len][name][8B size][4B mode]` |

### 4.3 Error Codes (for RESP_ERR)

| Code | Meaning |
|---|---|
| 404 | File not found |
| 403 | Permission denied |
| 409 | File already exists (on upload, if you choose no-overwrite policy) |
| 500 | Internal server error (I/O failure, mmap failure, etc.) |
| 400 | Malformed request / bad opcode |

### 4.4 Path Safety — Non-Negotiable

Every filename received from a client **must** be validated before touching the filesystem:
- Reject any name containing `..`, leading `/`, or null bytes.
- Resolve the final path with `realpath()` and confirm it's still inside your configured storage root before any `open()` call.
- This is the single most common way a student project turns into an arbitrary-file-read/write vulnerability. Do this in one central function (`fileops.c: safe_resolve_path()`) and route every handler through it — don't reimplement the check per-handler.

---

## 5. Build Phases

### Phase 0 — Skeleton & Sanity
- `main.c` parses a port number from argv, prints "NERV-FS starting on port X", exits cleanly.
- Set up `Makefile`, confirm `gcc` build works.
- **Test:** binary compiles and runs with no crash.

### Phase 1 — TCP Socket Bring-Up
- Implement in `net.c`: `socket(AF_INET, SOCK_STREAM, 0)` → `setsockopt(SO_REUSEADDR)` → `bind()` → `listen()`.
- Blocking `accept()` loop first (no threads, no multiplexing yet) — just accept one client, read whatever it sends, echo it back, close.
- **Test:** `nc localhost <port>`, type text, see it echoed.
- **Common pitfalls:** forgetting `htons()` on the port, forgetting to zero the `sockaddr_in` struct with `memset`, "Address already in use" errors (fix with `SO_REUSEADDR`).

### Phase 2 — Non-Blocking I/O Multiplexing
- Replace the single-client blocking loop with `select()` (simpler to start, `poll()` is a fine upgrade path if you want to demonstrate that too) watching the listening socket + all connected client sockets.
- On new connection: `accept()` and add fd to the watch set.
- On readable client fd: read available bytes (non-blocking).
- **Test:** connect 2–3 `nc` sessions simultaneously; confirm all are served without one blocking another.
- **Note for the writeup:** `select()` has an `FD_SETSIZE` limit (typically 1024) — worth one paragraph in your report on why `poll()` or `epoll()` scale further, even if you implement `select()` for simplicity.

### Phase 3 — Protocol Layer
- Implement `nervfs_header_t` parsing/serialization in `protocol.c`.
- Write a `recv_full(fd, buf, len)` helper that loops on `read()` until `len` bytes are collected or the connection closes — **`read()` on a TCP socket is not guaranteed to return all requested bytes in one call**, this is the #1 bug source in socket protocol code.
- **Test:** write a tiny client that sends a REQ_LIST header with zero payload; server parses the header correctly and prints the opcode.

### Phase 4 — Thread Pool
- In `threadpool.c`: fixed-size array of `pthread_t` workers (e.g. 8), a job queue (linked list or ring buffer) protected by `pthread_mutex_t`, and a `pthread_cond_t` that workers wait on when the queue is empty.
- Main `select()`/`accept()` loop becomes the **producer**: on a fully-read client request, it pushes a "job" (client fd + parsed request) onto the queue and signals the condition variable.
- Worker threads are the **consumers**: they lock the mutex, wait on the cond var if queue is empty, pop a job, unlock, process it.
- **Test:** connect 5+ clients simultaneously issuing REQ_LIST; confirm all get responses and no deadlock/crash under repeated runs (run 50+ times in a loop — thread bugs are often nondeterministic).

### Phase 5 — File Operations Engine
- `fileops.c`: wrap `open/read/write/close` for straightforward paths (small file writes, listing via `opendir`/`readdir`).
- Implement `REQ_UPLOAD` and `REQ_DOWNLOAD` handlers first using plain `read`/`write` — get correctness before optimizing.
- **Test:** upload a small text file from your test client, confirm it lands correctly in `storage/`; download it back, diff the bytes.

### Phase 6 — mmap-Based Zero-Copy Transfer
- For `REQ_DOWNLOAD`, replace the read-loop with `mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0)`, then `write()` (or a scatter/gather `writev`) directly from the mapped region to the socket — this avoids an extra copy into a user-space buffer.
- Always `munmap()` after use, and handle `mmap` returning `MAP_FAILED` (e.g. for zero-length files — mmap can't map a 0-byte file, special-case it).
- **Test:** upload/download a large file (100MB+) and confirm correctness (checksum before/after with `sha256sum`) and observe (via `time`) that transfer doesn't regress vs. the read/write version.

### Phase 7 — Locking & Concurrency Safety
- Two layers of locking, don't conflate them:
  - **In-process:** a `pthread_mutex_t` per shared in-memory structure (e.g. an index of "who's currently writing file X") to protect your own data structures from races between worker threads.
  - **Cross-process/file-level:** `fcntl(fd, F_SETLK, &flock_struct)` advisory locks on the actual file descriptor, so two uploads to the same filename don't corrupt each other, and a download-while-uploading either waits or gets a clean "locked" error rather than reading a half-written file.
- Decide and document your policy explicitly: e.g. "uploads take an exclusive `fcntl` write lock; downloads take a shared read lock; if a write lock is held, downloads block until it's released."
- **Test:** two clients uploading the *same* filename simultaneously — confirm no corrupted/interleaved output and a defined behavior (both succeed sequentially, or second one gets rejected — your choice, just be consistent).

### Phase 8 — Signals & Graceful Shutdown
- Install handlers for `SIGINT` (Ctrl+C) and `SIGPIPE` (client disconnects mid-write — **without handling this, your server will simply die** the first time a client closes a socket early, since the default action for `SIGPIPE` is process termination).
- `SIGPIPE`: simplest fix is `signal(SIGPIPE, SIG_IGN)` and instead check `write()`'s return value/`errno` (`EPIPE`) per-call.
- `SIGINT`: set a global `volatile sig_atomic_t running` flag, break the main accept loop, join all worker threads, close all open fds/sockets, then exit — a real "graceful shutdown," not just `exit()` from inside the handler (calling non-async-signal-safe functions like `printf` or `malloc` directly inside a signal handler is undefined-behavior-adjacent territory; set a flag and let the main loop notice it).
- **Test:** kill a client mid-transfer, confirm server logs the disconnect and stays alive; Ctrl+C the server, confirm clean log output and process exit (check with `echo $?`).

### Phase 9 — Diagnostics & Profiling Writeup
This maps directly to your syllabus's "Diagnostics & Profiling" requirement — budget real time for it, it's often worth marks separate from the working code:
- **`strace -f -e trace=network,file ./nervfs_server <port>`** — capture and annotate a snippet showing the `socket/bind/listen/accept` sequence and file syscalls per request. `-f` follows child threads.
- **`netstat -tnp` / `ss -tnp`** — show the listening socket and established connections while clients are attached; use this to demonstrate the thread-pool handling multiple simultaneous connections on one listening port.
- **`tcpdump -i lo -w capture.pcap port <port>`** then open in **Wireshark** — capture one full upload exchange, annotate the custom header bytes in the hex view (this is a great screenshot for a report: show your `0xAE` magic byte and opcode field visibly in the raw bytes).
- Write these up in `docs/diagnostics.md` with screenshots/output snippets and 2–3 sentences explaining what each shows.

---

## 6. Minimal Test Client

You'll want a simple CLI client (`client/nervfs_client.c`) early — don't wait until Phase 5. A basic version just needs to:
1. `connect()` to the server.
2. Build a `nervfs_header_t` + payload for a chosen opcode from argv (e.g. `./nervfs_client upload myfile.txt`).
3. Send it, then read and print the response.

This becomes your test harness for every phase above — invest an hour here early, it pays for itself immediately.

---

## 7. Suggested Milestone Checklist (for grading/demo)

- [ ] Server accepts TCP connections (Phase 1–2)
- [ ] Multiple clients handled concurrently without blocking each other (Phase 2, 4)
- [ ] Custom binary protocol parses correctly, framing is robust to partial reads (Phase 3)
- [ ] Upload / Download / List / Delete / Chmod all functional end-to-end (Phase 5)
- [ ] Large-file transfer uses `mmap` zero-copy path (Phase 6)
- [ ] Concurrent access to the same file is race-free (`fcntl` locks) (Phase 7)
- [ ] Clean shutdown on SIGINT; survives client crashes via SIGPIPE handling (Phase 8)
- [ ] `strace`/`netstat`/`tcpdump`+Wireshark evidence documented (Phase 9)
- [ ] Path traversal protection verified (Section 4.4) — try `../../etc/passwd` as a filename in testing and confirm it's rejected
- [ ] Node bridge translates REST↔binary protocol correctly for all five opcodes (Phase 10)
- [ ] React UI: file list, upload with progress bar, download, delete, permissions modal all working end-to-end (Phase 11)

---

## 8. Stretch Goals (only after everything above is solid)

- Swap `select()` for `epoll()` and compare/benchmark.
- Add a simple length-prefixed **resume/partial-download** (range requests) via an extra header field.
- Add TLS via a minimal library — explicitly out of scope for the "no third-party runtime" spirit of this project, mention as future work rather than implementing, unless your syllabus rewards it.
- Add authentication as a v2 (a `REQ_LOGIN` opcode + a per-connection authenticated flag is the natural extension point — the protocol above already leaves the `reserved` header field free for a session token later).

---

## 9. Key Gotchas Reference (skim this before each phase)

| Gotcha | Where it bites | Fix |
|---|---|---|
| `read()`/`write()` partial completion | Protocol parsing | Always loop until full length read/written or error |
| Struct padding in wire header | Cross-platform parsing | `#pragma pack(push,1)` |
| `SIGPIPE` default = process death | Any client disconnect | `signal(SIGPIPE, SIG_IGN)`, check `errno==EPIPE` |
| `mmap` on 0-byte files | Downloads of empty files | Special-case file_size==0, skip mmap |
| Path traversal (`../`) | Every filename-taking opcode | Centralized `safe_resolve_path()` + `realpath()` check |
| Race between threads on shared index | Concurrent uploads/deletes | `pthread_mutex_t` around the in-memory index |
| Race between processes/fds on same file | Concurrent upload+download of same name | `fcntl(F_SETLK)` advisory locks |
| Forgetting `-pthread` in Makefile | Silent thread bugs | Always in `CFLAGS` |
| `select()`'s 1024-fd ceiling | Many simultaneous clients | Document as a known limitation, mention `poll`/`epoll` |

---

## 10. Web Frontend Architecture (Phase 10–11, after core server is done)

### 10.1 Why You Need a Bridge

Browsers cannot open raw TCP sockets or speak arbitrary binary protocols directly — JavaScript in the browser is limited to HTTP(S), WebSockets, and a few other browser-sanctioned transports. Your NERV-FS wire protocol (Section 4) is a hand-rolled binary format over plain TCP, which a browser simply has no API for.

The fix is a three-tier stack:

```
┌─────────────┐   REST/JSON over HTTP   ┌──────────────┐   Custom binary protocol   ┌──────────────┐
│ React (UI)  │ ─────────────────────▶  │ Node Bridge  │ ─────────────────────────▶ │ C NERV-FS    │
│ browser     │ ◀─────────────────────  │ (Express)    │ ◀───────────────────────── │ server       │
└─────────────┘                         └──────────────┘         (Section 4)        └──────────────┘
```

The **C server does not change at all.** The bridge is a small Node.js process that:
1. Exposes normal REST endpoints (`GET /files`, `POST /files`, `DELETE /files/:name`, `PATCH /files/:name/permissions`, `GET /files/:name` for download).
2. For each request, opens a TCP connection to the C server, encodes it as a `nervfs_header_t` + payload exactly like your CLI test client does, sends it, and parses the binary response.
3. Translates that response into JSON (or a file stream, for downloads) and sends it back to React.

This is standard practice — it's the same pattern used when any modern web frontend needs to talk to a legacy or non-HTTP backend (databases, message queues, custom RPC services all work this way).

### 10.2 Bridge Implementation (`bridge/`)

**Dependencies:**
```bash
cd bridge && npm init -y && npm install express cors multer
```

**`bridge/nervfsClient.js`** — the core piece. It re-implements Section 4's framing in JavaScript using Node's `net` module:

```javascript
const net = require('net');

const OPCODES = {
  REQ_UPLOAD: 0x01, REQ_DOWNLOAD: 0x02, REQ_LIST: 0x03,
  REQ_DELETE: 0x04, REQ_CHMOD: 0x05,
};

function buildHeader(opcode, payloadLen) {
  const buf = Buffer.alloc(16);
  buf.writeUInt8(0xAE, 0);        // magic
  buf.writeUInt8(1, 1);           // version
  buf.writeUInt16BE(opcode, 2);
  buf.writeUInt32BE(payloadLen, 4);
  buf.writeBigUInt64BE(0n, 8);    // reserved
  return buf;
}

function sendRequest(host, port, opcode, payload) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host, port }, () => {
      socket.write(Buffer.concat([buildHeader(opcode, payload.length), payload]));
    });

    let chunks = [];
    socket.on('data', (d) => chunks.push(d));
    socket.on('end', () => resolve(Buffer.concat(chunks)));
    socket.on('error', reject);
  });
}

module.exports = { sendRequest, OPCODES };
```

Note: this is a simplified single-shot version. Because TCP can deliver a response across multiple `data` events, and because a real implementation needs to read the 16-byte header first to know exactly how many payload bytes to expect (same partial-read problem as Section 9), for anything beyond a class demo you'll want a small buffering reader here — mirror the `recv_full()` logic from your C client, just in JS.

**`bridge/server.js`** — REST endpoints, e.g.:

```javascript
const express = require('express');
const multer = require('multer');
const cors = require('cors');
const { sendRequest, OPCODES } = require('./nervfsClient');

const app = express();
app.use(cors());
const upload = multer(); // handles multipart form uploads, keeps file in memory/buffer

const NERVFS_HOST = '127.0.0.1';
const NERVFS_PORT = 9000;

app.get('/files', async (req, res) => {
  const resp = await sendRequest(NERVFS_HOST, NERVFS_PORT, OPCODES.REQ_LIST, Buffer.alloc(0));
  // parse RESP_LIST_DATA payload into JSON here, then:
  res.json({ files: [] /* parsed entries */ });
});

app.post('/files', upload.single('file'), async (req, res) => {
  const nameBuf = Buffer.from(req.file.originalname, 'utf8');
  const header = Buffer.alloc(2 + nameBuf.length + 8);
  header.writeUInt16BE(nameBuf.length, 0);
  nameBuf.copy(header, 2);
  header.writeBigUInt64BE(BigInt(req.file.size), 2 + nameBuf.length);
  const payload = Buffer.concat([header, req.file.buffer]);
  await sendRequest(NERVFS_HOST, NERVFS_PORT, OPCODES.REQ_UPLOAD, payload);
  res.json({ status: 'ok' });
});

// GET /files/:name, DELETE /files/:name, PATCH /files/:name/permissions follow the same pattern

app.listen(4000, () => console.log('NERV-FS bridge listening on :4000'));
```

**Test:** `curl http://localhost:4000/files` while your C server is running — confirm you get back JSON, not a hang or error.

### 10.3 React Frontend (`frontend/`)

**Setup:**
```bash
cd frontend && npm create vite@latest . -- --template react
npm install axios
```

**Component breakdown:**

| Component | Responsibility |
|---|---|
| `App.jsx` | Layout, holds the file list state, refreshes it after any mutation |
| `FileList.jsx` | Renders files as a table (name, size, permissions), download/delete/permissions buttons per row |
| `UploadForm.jsx` | File `<input type="file">`, drag-and-drop optional, triggers upload with progress tracking |
| `ProgressBar.jsx` | Simple `<progress>`-based bar, driven by upload percentage state |
| `PermissionsModal.jsx` | Small modal for setting a numeric mode (e.g. `644`, `755`) — calls the PATCH endpoint |
| `api/nervfsApi.js` | Thin axios wrapper around the bridge's REST endpoints — keeps all fetch/axios calls in one place |

**Progress bars — how they actually work here:** since the bridge does a normal HTTP `multipart/form-data` upload, you get real byte-level progress for free via axios's `onUploadProgress`, which tracks the browser→bridge leg (the large majority of the transfer time for anything but tiny files):

```javascript
// api/nervfsApi.js
import axios from 'axios';

const BASE = 'http://localhost:4000';

export const listFiles = () => axios.get(`${BASE}/files`);

export const uploadFile = (file, onProgress) => {
  const form = new FormData();
  form.append('file', file);
  return axios.post(`${BASE}/files`, form, {
    onUploadProgress: (e) => onProgress(Math.round((e.loaded * 100) / e.total)),
  });
};

export const deleteFile = (name) => axios.delete(`${BASE}/files/${encodeURIComponent(name)}`);

export const setPermissions = (name, mode) =>
  axios.patch(`${BASE}/files/${encodeURIComponent(name)}/permissions`, { mode });

export const downloadFile = (name) => {
  window.location.href = `${BASE}/files/${encodeURIComponent(name)}`;
};
```

The bridge→C-server leg (Section 10.2) is local/loopback traffic and typically fast enough not to need its own progress reporting for a class project; note this as a known simplification in your report if asked.

### 10.4 Running the Full Stack

Three processes, three terminals, in this order:

```bash
# Terminal 1 — the C engine
./nervfs_server 9000

# Terminal 2 — the bridge
cd bridge && node server.js

# Terminal 3 — the React dev server
cd frontend && npm run dev
```

Then open the Vite dev server URL (typically `http://localhost:5173`) in a browser.

### 10.5 What to Say in Your Report About the Frontend

Since your syllabus is graded on systems programming, be explicit in your writeup that the React/Node layer is a **presentation/demo convenience**, not where the systems-programming work lives — the grading-relevant engineering (sockets, threads, mmap, fcntl, signals) is entirely in the C server, untouched by adding this UI. This framing also protects you if an evaluator wonders why a "systems project" has a Node dependency at all.
