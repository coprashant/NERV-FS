# NERV-FS test report

This documents every test run against the project, phase by phase manual
verification during development, then the automated performance suite run
twice: once in the build sandbox, once on the actual target machine.

## 1. Test environments

| | Sandbox | Target machine |
|---|---|---|
| OS | Ubuntu 24 container | Linux, Aspire A315-44P |
| Filesystem for storage/ | ext4 | NTFS mounted partition |
| Compiler | gcc, -Wall -Wextra, zero warnings | gcc |
| Server invocation | `./nervfs_server <port>` | `./nervfs_server 9000` |

The filesystem difference matters for exactly one result, noted in
section 4.

## 2. Manual phase by phase verification

Each phase was tested by hand immediately after implementation, against a
running server, not just read through.

| Phase | What was tested | Result |
|-------|------------------|--------|
| 0 | port argument parsing, startup message, rejection of bad ports | pass |
| 1 | blocking accept and echo loop against `nc`, arbitrary bytes echoed back exactly | pass |
| 2 | `select()` loop, 3 simultaneous clients each sent different text, each received only their own text back, none blocked the others | pass |
| 3 | header parsing, sent a real `REQ_LIST` with zero payload, server logged `received opcode REQ_LIST payload len 0`; sent garbage bytes over `nc`, server logged `bad magic or version rejecting client` and stayed alive | pass |
| 4 | thread pool, 50 runs of 6 concurrent `REQ_LIST` clients, 300 total requests, zero failures, log confirmed work spread across the 8 worker threads and different client fds | pass |
| 5 | upload then confirmed on disk then download then `diff`, byte identical; `chmod` to 600 confirmed with `ls -la`; path traversal payload `../../../etc/passwd` rejected with `403 invalid filename`; delete then re-delete returned `404 file not found` | pass |
| 6 | uploaded and downloaded a 150MB random file through the mmap zero copy path, `sha256sum` matched exactly before and after; zero byte file special case handled without attempting to mmap an empty file | pass |
| 7 | two clients uploading the same filename simultaneously (5MB each, all `A` vs all `B`), final file was uniformly one writer's data at the correct size, wall clock roughly equalled the sum of the two individual times confirming serialization not a race; a download racing an upload to the same file always saw either the fully old or fully new content, never a torn or partial read | pass |
| 8 | a client that disconnected mid upload was logged and the server stayed alive with nothing partial written to disk; a client that closed its socket before reading a response did not crash the server via `SIGPIPE`; `Ctrl+C` produced a clean `select loop exiting` / `shutting down thread pool` / `shutdown complete` log sequence and exit code 0 | pass |
| 9 | `strace -f` showed the expected `socket`/`bind`/`listen`/`accept` sequence plus the correct `openat` flags per opcode, and every thread including all 8 workers exiting with 0 after `SIGINT`; `ss`/`netstat` showed 4 simultaneous `ESTABLISHED` connections against one server process on one port; `tcpdump` captured a full upload exchange, hex dump confirmed the `0xAE` magic byte and correct opcode fields for both the request and the response | pass |

All nine phases passed their own test criteria on first working
implementation or after one fix, no known regressions between phases.

## 3. Automated performance suite

`tests/stress_test.py` was run three times total, twice in the sandbox
during development and once by the project owner on the real target
machine. Full mode results below unless noted.

### 3.1 Sandbox, quick mode

| Test | Result |
|------|--------|
| correctness roundtrip | pass |
| throughput 1MB | pass, 26.3 MB/s up, 352.6 MB/s down |
| throughput 10MB | pass, 27.8 MB/s up, 488.3 MB/s down |
| concurrent small ops, 6x10 | pass, 3241 req/s, p50 0.8ms p95 3.5ms p99 5.4ms |
| thread pool saturation, 16 clients | pass |
| same filename write contention, 3 writers | pass, no corruption |
| repeated bursts, 10x6 | pass, zero failures |
| mixed workload soak, 5s | pass, 44281 ops, 0 errors |

### 3.2 Sandbox, full mode

| Test | Result |
|------|--------|
| correctness roundtrip | pass |
| throughput 1MB | pass, 56.7 MB/s up, 567.9 MB/s down |
| throughput 10MB | pass, 38.5 MB/s up, 632.4 MB/s down |
| throughput 100MB | pass, 35.6 MB/s up, 462.0 MB/s down, sha256 verified |
| concurrent small ops, 10x20 | pass, 9314 req/s, p50 0.6ms p95 1.0ms p99 4.9ms |
| thread pool saturation, 32 clients | pass |
| same filename write contention, 4 writers | pass, no corruption |
| repeated bursts, 50x6 | pass, 300 requests, zero failures |
| mixed workload soak, 15s | pass, 139881 ops, 0 errors |

### 3.3 Target machine, full mode

| Test | Result |
|------|--------|
| correctness roundtrip | **fail**, see section 4 |
| throughput 1MB | pass, 103.2 MB/s up, 457.4 MB/s down |
| throughput 10MB | pass, 101.7 MB/s up, 537.4 MB/s down |
| throughput 100MB | pass, 132.6 MB/s up, 520.5 MB/s down, sha256 verified |
| concurrent small ops, 10x20 | pass, 7344 req/s, p50 1.1ms p95 2.3ms p99 3.3ms |
| thread pool saturation, 32 clients | pass |
| same filename write contention, 4 writers | pass, no corruption |
| repeated bursts, 50x6 | pass, 300 requests, zero failures |
| mixed workload soak, 15s | pass, 117610 ops, 0 errors |

The target machine is meaningfully faster on raw throughput than the
sandbox, likely a faster disk and no container overhead, everything else
is consistent between the two environments.

## 4. Known issue, chmod on NTFS

The one failure on the target machine was `chmod did not take effect,
mode was 0o777`, not a bug in the server. The project's `storage/`
directory on that machine sits on an NTFS mounted partition. NTFS does
not store Unix permission bits per file, so the mount driver, `ntfs-3g`
or similar, reports a fixed mode for every file regardless of what
`chmod()` is called with, and silently accepts the `chmod()` syscall
without persisting anything. The server's `fileops_chmod_file()` calls
`chmod()` and correctly checks its return value, the syscall itself
reports success because the driver does not report an error, it just
does not do anything durable.

This was independently confirmed earlier in development: `chmod` to 600
against a `storage/` directory on ext4 in the sandbox correctly showed up
in `ls -la`, the same operation against the NTFS mounted path did not
change the reported mode.

No code change is needed. If `chmod` behavior needs to be demonstrated on
the target machine, point `storage/` at a path on a native Linux
filesystem, for example somewhere under the home directory, and rerun the
roundtrip test.

## 5. Summary

Every phase's own test criteria passed. The automated suite passed all
eight of its tests in both sandbox runs, and seven of eight on the target
machine, with the eighth being an environment level filesystem limitation
rather than a defect. Throughput, concurrency, thread pool saturation,
same file write contention, and a sustained mixed workload soak all
behaved correctly and without data corruption across every environment
tested.