# NERV-FS
### Network-Enabled Efficient Remote Virtual File System

A concurrent, native C, self-hosted network file server for Linux — built from scratch using only POSIX system calls, no HTTP libraries, no web frameworks, no third-party runtime.

---

## Overview

NERV-FS is a multi-threaded TCP daemon that lets multiple clients upload, download, list, and manage files on a remote host. It speaks a custom binary wire protocol over plain TCP, handles concurrent connections via a fixed thread pool, and uses `mmap`-based zero-copy transfers for large file downloads.

A three-tier web stack sits on top of the core engine for browser access:

```
┌─────────────┐   REST/JSON over HTTP   ┌──────────────┐   Custom binary protocol   ┌──────────────┐
│  React UI   │ ──────────────────────▶ │ Node Bridge  │ ─────────────────────────▶ │  C Server    │
│  (browser)  │ ◀──────────────────────  │  (Express)   │ ◀───────────────────────── │  (NERV-FS)   │
└─────────────┘                         └──────────────┘                            └──────────────┘
     :5173                                   :4000                                       :9000
```

The C server is the grading-relevant core. The Node bridge and React UI are a demo convenience layer — the browser cannot speak raw TCP or custom binary protocols directly, so the bridge translates REST calls into the wire protocol on the frontend's behalf.

---

## Features

- **Custom binary wire protocol** — fixed 16-byte header (`magic`, `version`, `opcode`, `payload_len`) + variable payload; `#pragma pack(1)` for portable framing
- **Thread pool** — fixed-size worker array with a mutex/condvar-protected job queue; main accept loop is the producer, workers are consumers
- **`select()`-based I/O multiplexing** — single listening socket + all connected client fds multiplexed without one blocking another
- **`mmap` zero-copy downloads** — large file reads mapped directly into the socket write path, avoiding a user-space buffer copy
- **`fcntl` advisory file locking** — shared read locks for downloads, exclusive write locks for uploads; concurrent access to the same file is race-free
- **Path traversal protection** — all client-supplied filenames validated against the storage root via `realpath()`; `../` and absolute paths rejected centrally in `safe_resolve_path()`
- **Graceful shutdown** — `SIGINT` sets a flag and drains the thread pool; `SIGPIPE` is ignored and `EPIPE` checked per-write so a dead client doesn't kill the server
- **CLI test client** — `client/nervfs_client.c` exercises every opcode from the command line
- **React web UI** — file list, upload with real-time progress bar, download, delete, and a permissions modal
- **Node.js REST bridge** — translates the five REST endpoints into NERV-FS binary frames and back

---

## Project Structure

```
nerv-fs/
├── Makefile
├── include/
│   ├── protocol.h        # nervfs_header_t, opcode constants
│   ├── server.h          # server-wide structs and globals
│   ├── threadpool.h
│   ├── fileops.h
│   ├── handlers.h
│   ├── net.h
│   └── logging.h
├── src/
│   ├── main.c            # entrypoint, arg parsing, signal setup
│   ├── net.c             # socket, bind, listen, accept, select loop
│   ├── threadpool.c      # worker pool, job queue, mutex/condvar
│   ├── protocol.c        # message parsing/serialization, recv_full()
│   ├── fileops.c         # open/read/write/mmap/fcntl wrappers, safe_resolve_path()
│   ├── handlers.c        # UPLOAD/DOWNLOAD/LIST/DELETE/CHMOD logic
│   └── logging.c
├── client/
│   └── nervfs_client.c   # minimal CLI client
├── bridge/               # Node.js REST ↔ binary-protocol gateway
│   ├── package.json
│   ├── server.js         # Express REST endpoints
│   └── nervfsClient.js   # speaks the wire protocol over TCP (Node net module)
├── frontend/             # React web UI
│   ├── package.json
│   └── src/
│       ├── App.jsx
│       ├── api/nervfsApi.js          # axios wrapper around bridge endpoints
│       └── components/
│           ├── FileList.jsx          # file table with download/delete/permissions buttons
│           ├── UploadForm.jsx        # file picker with drag-and-drop
│           ├── ProgressBar.jsx       # upload progress
│           └── PermissionsModal.jsx  # numeric mode input (e.g. 644, 755)
├── storage/              # sandboxed file root (gitignored)
├── tests/
│   ├── test_upload.sh
│   ├── test_concurrent_clients.sh
│   └── stress_test.py
└── docs/
    ├── documentation.md
    ├── protocol_spec.md
    ├── api_reference.md
    └── diagnostics.md
```

---

## Wire Protocol

Every message starts with a **16-byte fixed header**, followed by `payload_len` bytes of body:

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;        // 0xAE — stream sync sanity check
    uint8_t  version;      // protocol version (currently 1)
    uint16_t opcode;       // operation code
    uint32_t payload_len;  // body length in bytes
    uint64_t reserved;     // future use (session token, flags) — zero for now
} nervfs_header_t;
#pragma pack(pop)
```

| Opcode | Name | Direction | Payload |
|---|---|---|---|
| `0x01` | `REQ_UPLOAD` | C→S | `[2B name_len][name][8B file_size][file bytes]` |
| `0x02` | `REQ_DOWNLOAD` | C→S | `[2B name_len][name]` |
| `0x03` | `REQ_LIST` | C→S | *(empty)* |
| `0x04` | `REQ_DELETE` | C→S | `[2B name_len][name]` |
| `0x05` | `REQ_CHMOD` | C→S | `[2B name_len][name][4B mode]` |
| `0x80` | `RESP_OK` | S→C | operation-dependent |
| `0x81` | `RESP_ERR` | S→C | `[2B err_code][2B msg_len][msg]` |
| `0x82` | `RESP_FILE_DATA` | S→C | `[8B file_size][file bytes]` |
| `0x83` | `RESP_LIST_DATA` | S→C | `[4B entry_count][entries: 2B name_len + name + 8B size + 4B mode]` |

---

## Building & Running

### Get the Source

**Clone** (read-only copy, push requires access):
```bash
git clone https://github.com/coprashant/NERV-FS.git
cd NERV-FS
```

**Fork** (your own copy on GitHub to freely modify and send PRs from):
1. Click **Fork** at the top-right of [github.com/coprashant/NERV-FS](https://github.com/coprashant/NERV-FS).
2. Clone your fork:
```bash
git clone https://github.com/<your-username>/NERV-FS.git
cd NERV-FS
```
3. Add the original as an upstream remote to pull in future changes:
```bash
git remote add upstream https://github.com/coprashant/NERV-FS.git
git fetch upstream
```

To sync your fork with upstream later:
```bash
git checkout main
git pull upstream main
git push origin main
```

### Dependencies

```bash
sudo apt update
sudo apt install build-essential gdb strace tcpdump wireshark netcat-openbsd
```

### Build the C server

```bash
make
```

This produces `nervfs_server`. The Makefile compiles with `-Wall -Wextra -pthread -g -D_GNU_SOURCE`.

### Run the full stack

Three terminals, in order:

```bash
# Terminal 1 — C engine
./nervfs_server 9000

# Terminal 2 — Node bridge
cd bridge && npm install && node server.js

# Terminal 3 — React dev server
cd frontend && npm install && npm run dev
```

Open `http://localhost:5173` in a browser.

### CLI client

```bash
gcc -Iinclude -o nervfs_client client/nervfs_client.c
./nervfs_client list
./nervfs_client upload myfile.txt
./nervfs_client download myfile.txt
./nervfs_client delete myfile.txt
./nervfs_client chmod myfile.txt 644
```

---

## Configuration

| Setting | Default | Where to change |
|---|---|---|
| Server port | `9000` | argv to `./nervfs_server` |
| Thread pool size | `8` | `threadpool.h` / `threadpool.c` |
| Storage root | `./storage/` | `fileops.c` |
| Bridge → server host/port | `127.0.0.1:9000` | `bridge/server.js` |
| Bridge listen port | `4000` | `bridge/server.js` |

---

## Testing

```bash
# Basic upload/download round-trip
bash tests/test_upload.sh

# Concurrent client connections
bash tests/test_concurrent_clients.sh

# Stress test (Python, requires requests library)
pip install requests
python3 tests/stress_test.py
```

Large-file integrity check:
```bash
dd if=/dev/urandom of=/tmp/bigfile bs=1M count=200
./nervfs_client upload /tmp/bigfile
./nervfs_client download bigfile /tmp/bigfile_out
sha256sum /tmp/bigfile /tmp/bigfile_out   # must match
```

---

## Security Notes

- **No authentication** — every connected client has full read/write access to the storage root. Never point the storage root at a real home directory or sensitive path.
- **Path traversal protection** is enforced centrally in `safe_resolve_path()` (`fileops.c`). All filename-taking handlers route through it; filenames containing `..`, a leading `/`, or null bytes are rejected before any `open()` call.
- Intended for sandboxed/demo use. Authentication (a `REQ_LOGIN` opcode + per-connection session flag in the reserved header field) is documented as a v2 extension.

---

## Known Limitations

- `select()` has an `FD_SETSIZE` ceiling of 1024 file descriptors; beyond ~1000 simultaneous clients, switch to `poll()` or `epoll()`.
- Upload progress in the browser UI tracks the browser→bridge leg only (loopback bridge→server traffic is fast enough to treat as instantaneous for typical use).
- `mmap` cannot map zero-byte files; empty file downloads take the plain `read`/`write` fallback path.

---

## Tech Stack

| Layer | Technology |
|---|---|
| Core server | C (POSIX), GCC, pthreads |
| IPC/locking | `pthread_mutex_t`, `pthread_cond_t`, `fcntl(F_SETLK)` |
| I/O | `select()`, `mmap`, `read`/`write`, `opendir`/`readdir` |
| REST bridge | Node.js, Express, multer, cors |
| Frontend | React (Vite), axios |

---

## Contributors

| Name | Website | GitHub |
|---|---|---|
| Prasant Bhattarai | [prasant-bhattarai.com.np](https://www.prasant-bhattarai.com.np) | [coprashant](https://github.com/coprashant) |
| Bikesh Sah | [bikeshsah.com.np](https://bikeshsah.com.np/) | [bikesh19](https://github.com/bikesh19) |
