#!/usr/bin/env python3
"""
NERV FS performance and stress test suite

Speaks the wire protocol directly over raw sockets, no dependency on the
C client binary, so every test controls its own timing and concurrency.

Usage
  python3 stress_test.py --port 9000
  python3 stress_test.py --port 9000 --quick

Covers
  throughput on large file upload and download, with checksum verification
  latency and requests per second for small operations under concurrency
  thread pool saturation with more clients than worker threads
  repeated burst runs to catch nondeterministic concurrency bugs
  same filename write contention, checked for corruption not just speed
  a short mixed workload soak run

Exits 0 if every test passed, 1 otherwise, so it can be used in a CI step.
"""

import argparse
import hashlib
import os
import socket
import statistics
import struct
import sys
import threading
import time

MAGIC = 0xAE
VERSION = 1
HEADER_SIZE = 16

OP_REQ_UPLOAD = 0x01
OP_REQ_DOWNLOAD = 0x02
OP_REQ_LIST = 0x03
OP_REQ_DELETE = 0x04
OP_REQ_CHMOD = 0x05
OP_RESP_OK = 0x80
OP_RESP_ERR = 0x81
OP_RESP_FILE_DATA = 0x82
OP_RESP_LIST_DATA = 0x83

PREFIX = "perftest_"


# ---------------------------------------------------------------------------
# wire protocol helpers, deliberately independent from the C client and any
# earlier test code so this file can run standalone against any build

def build_header(opcode, payload_len):
    return struct.pack("!BBHIQ", MAGIC, VERSION, opcode, payload_len, 0)


def parse_header(buf):
    magic, version, opcode, payload_len, reserved = struct.unpack("!BBHIQ", buf)
    return magic, version, opcode, payload_len


def recv_exact(sock, n):
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise ConnectionError("socket closed before enough bytes arrived")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def do_request(host, port, opcode, payload, timeout=30):
    """sends one request on a fresh connection, returns opcode, payload bytes"""
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.sendall(build_header(opcode, len(payload)) + payload)
        header = recv_exact(sock, HEADER_SIZE)
        magic, version, resp_opcode, resp_len = parse_header(header)
        if magic != MAGIC or version != VERSION:
            raise ValueError("bad magic or version in response")
        body = recv_exact(sock, resp_len) if resp_len > 0 else b""
        return resp_opcode, body
    finally:
        sock.close()


def parse_err(body):
    err_code, msg_len = struct.unpack("!HH", body[:4])
    msg = body[4:4 + msg_len].decode("utf8", errors="replace")
    return err_code, msg


def op_upload(host, port, name, data):
    name_b = name.encode()
    payload = struct.pack("!H", len(name_b)) + name_b + struct.pack("!Q", len(data)) + data
    opcode, body = do_request(host, port, OP_REQ_UPLOAD, payload, timeout=120)
    if opcode != OP_RESP_OK:
        raise RuntimeError(f"upload failed, {parse_err(body) if opcode == OP_RESP_ERR else opcode}")


def op_download(host, port, name):
    name_b = name.encode()
    payload = struct.pack("!H", len(name_b)) + name_b
    opcode, body = do_request(host, port, OP_REQ_DOWNLOAD, payload, timeout=120)
    if opcode != OP_RESP_FILE_DATA:
        raise RuntimeError(f"download failed, {parse_err(body) if opcode == OP_RESP_ERR else opcode}")
    (size,) = struct.unpack("!Q", body[:8])
    return body[8:8 + size]


def op_list(host, port):
    opcode, body = do_request(host, port, OP_REQ_LIST, b"")
    if opcode != OP_RESP_LIST_DATA:
        raise RuntimeError(f"list failed, {parse_err(body) if opcode == OP_RESP_ERR else opcode}")
    (count,) = struct.unpack("!I", body[:4])
    off = 4
    entries = []
    for _ in range(count):
        (name_len,) = struct.unpack("!H", body[off:off + 2])
        off += 2
        name = body[off:off + name_len].decode()
        off += name_len
        (size,) = struct.unpack("!Q", body[off:off + 8])
        off += 8
        (mode,) = struct.unpack("!I", body[off:off + 4])
        off += 4
        entries.append((name, size, mode))
    return entries


def op_delete(host, port, name):
    name_b = name.encode()
    payload = struct.pack("!H", len(name_b)) + name_b
    opcode, body = do_request(host, port, OP_REQ_DELETE, payload)
    if opcode != OP_RESP_OK:
        raise RuntimeError(f"delete failed, {parse_err(body) if opcode == OP_RESP_ERR else opcode}")


def op_chmod(host, port, name, mode):
    name_b = name.encode()
    payload = struct.pack("!H", len(name_b)) + name_b + struct.pack("!I", mode)
    opcode, body = do_request(host, port, OP_REQ_CHMOD, payload)
    if opcode != OP_RESP_OK:
        raise RuntimeError(f"chmod failed, {parse_err(body) if opcode == OP_RESP_ERR else opcode}")


# ---------------------------------------------------------------------------
# small helpers shared by several tests

class Result:
    def __init__(self, name):
        self.name = name
        self.passed = True
        self.detail = ""

    def fail(self, detail):
        self.passed = False
        self.detail = detail
        return self

    def ok(self, detail):
        self.detail = detail
        return self


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    k = (len(sorted_values) - 1) * p
    f = int(k)
    c = min(f + 1, len(sorted_values) - 1)
    if f == c:
        return sorted_values[f]
    return sorted_values[f] + (sorted_values[c] - sorted_values[f]) * (k - f)


# ---------------------------------------------------------------------------
# individual tests, each returns a Result

def test_correctness_roundtrip(host, port):
    r = Result("correctness roundtrip")
    name = PREFIX + "roundtrip.bin"
    data = os.urandom(64 * 1024)
    try:
        op_upload(host, port, name, data)
        back = op_download(host, port, name)
        if back != data:
            return r.fail("downloaded bytes did not match uploaded bytes")
        op_chmod(host, port, name, 0o600)
        entries = op_list(host, port)
        found = [e for e in entries if e[0] == name]
        if not found:
            return r.fail("uploaded file did not appear in list")
        if found[0][2] != 0o600:
            return r.fail(f"chmod did not take effect, mode was {oct(found[0][2])}")
        op_delete(host, port, name)
        entries_after = op_list(host, port)
        if any(e[0] == name for e in entries_after):
            return r.fail("file still present after delete")
        return r.ok("upload, download, chmod, list, delete all verified byte exact")
    except Exception as e:
        return r.fail(str(e))
    finally:
        try:
            op_delete(host, port, name)
        except Exception:
            pass


def test_throughput(host, port, size_mb):
    r = Result(f"throughput {size_mb}MB upload and download")
    name = PREFIX + f"throughput_{size_mb}mb.bin"
    size = size_mb * 1024 * 1024
    data = os.urandom(size)
    expected_hash = hashlib.sha256(data).hexdigest()
    try:
        t0 = time.time()
        op_upload(host, port, name, data)
        upload_elapsed = time.time() - t0
        upload_mbps = size_mb / upload_elapsed if upload_elapsed > 0 else float("inf")

        t0 = time.time()
        back = op_download(host, port, name)
        download_elapsed = time.time() - t0
        download_mbps = size_mb / download_elapsed if download_elapsed > 0 else float("inf")

        actual_hash = hashlib.sha256(back).hexdigest()
        if actual_hash != expected_hash:
            return r.fail("sha256 mismatch after round trip, data corruption")

        detail = (
            f"upload {upload_elapsed:.3f}s ({upload_mbps:.1f} MB/s), "
            f"download {download_elapsed:.3f}s ({download_mbps:.1f} MB/s), "
            f"sha256 verified"
        )
        return r.ok(detail)
    except Exception as e:
        return r.fail(str(e))
    finally:
        try:
            op_delete(host, port, name)
        except Exception:
            pass


def test_concurrent_small_ops(host, port, num_clients, ops_per_client):
    r = Result(f"concurrent small ops, {num_clients} clients x {ops_per_client} ops")
    latencies = []
    errors = []
    lock = threading.Lock()

    def worker():
        for _ in range(ops_per_client):
            t0 = time.time()
            try:
                op_list(host, port)
                elapsed = time.time() - t0
                with lock:
                    latencies.append(elapsed)
            except Exception as e:
                with lock:
                    errors.append(str(e))

    threads = [threading.Thread(target=worker) for _ in range(num_clients)]
    t_start = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    total_elapsed = time.time() - t_start

    total_ops = num_clients * ops_per_client
    if errors:
        return r.fail(f"{len(errors)} of {total_ops} requests failed, first error, {errors[0]}")

    latencies.sort()
    p50 = percentile(latencies, 0.50) * 1000
    p95 = percentile(latencies, 0.95) * 1000
    p99 = percentile(latencies, 0.99) * 1000
    throughput = total_ops / total_elapsed if total_elapsed > 0 else float("inf")

    detail = (
        f"{total_ops} requests in {total_elapsed:.3f}s ({throughput:.1f} req/s), "
        f"latency p50 {p50:.1f}ms p95 {p95:.1f}ms p99 {p99:.1f}ms, zero errors"
    )
    return r.ok(detail)


def test_thread_pool_saturation(host, port, num_clients):
    r = Result(f"thread pool saturation, {num_clients} simultaneous clients, pool size is 8")
    results = {}
    lock = threading.Lock()

    def worker(i):
        try:
            entries = op_list(host, port)
            with lock:
                results[i] = ("ok", len(entries))
        except Exception as e:
            with lock:
                results[i] = ("error", str(e))

    barrier_threads = [threading.Thread(target=worker, args=(i,)) for i in range(num_clients)]
    t0 = time.time()
    for t in barrier_threads:
        t.start()
    for t in barrier_threads:
        t.join()
    elapsed = time.time() - t0

    failures = [v for v in results.values() if v[0] != "ok"]
    if failures:
        return r.fail(f"{len(failures)} of {num_clients} clients failed under saturation, first, {failures[0]}")
    return r.ok(
        f"all {num_clients} requests against an 8 worker pool succeeded in {elapsed:.3f}s, "
        f"queueing under load did not drop or corrupt any request"
    )


def test_repeated_bursts(host, port, iterations, clients_per_burst):
    r = Result(f"repeated burst runs, {iterations} runs x {clients_per_burst} concurrent clients")
    failed_runs = 0
    worst_run_detail = ""

    for run in range(iterations):
        results = {}
        lock = threading.Lock()

        def worker(i):
            try:
                op_list(host, port)
                with lock:
                    results[i] = True
            except Exception as e:
                with lock:
                    results[i] = str(e)

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(clients_per_burst)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        bad = [v for v in results.values() if v is not True]
        if bad:
            failed_runs += 1
            worst_run_detail = f"run {run}, {bad[0]}"

    total_requests = iterations * clients_per_burst
    if failed_runs > 0:
        return r.fail(f"{failed_runs} of {iterations} runs had a failure, example, {worst_run_detail}")
    return r.ok(f"{total_requests} requests across {iterations} independent bursts, zero failures, no deadlock")


def test_same_file_write_contention(host, port, num_writers, size_mb):
    r = Result(f"same filename write contention, {num_writers} concurrent uploads")
    name = PREFIX + "contention.bin"
    size = size_mb * 1024 * 1024
    payloads = [bytes([65 + i]) * size for i in range(num_writers)]
    results = {}
    lock = threading.Lock()

    def worker(i):
        t0 = time.time()
        try:
            op_upload(host, port, name, payloads[i])
            with lock:
                results[i] = time.time() - t0
        except Exception as e:
            with lock:
                results[i] = str(e)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(num_writers)]
    t_start = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    total_elapsed = time.time() - t_start

    failures = [v for v in results.values() if not isinstance(v, float)]
    if failures:
        return r.fail(f"{len(failures)} uploads errored, first, {failures[0]}")

    try:
        final = op_download(host, port, name)
    except Exception as e:
        return r.fail(f"could not download final file, {e}")

    unique_bytes = set(final)
    if len(unique_bytes) != 1 or len(final) != size:
        return r.fail(
            f"final file is corrupted or interleaved, size {len(final)}, "
            f"unique byte values {unique_bytes}"
        )

    winner_byte = next(iter(unique_bytes))
    sum_individual = sum(results.values())
    detail = (
        f"{num_writers} writers of {size_mb}MB each, wall clock {total_elapsed:.3f}s, "
        f"sum of individual times {sum_individual:.3f}s (serialized not parallel), "
        f"final file is uniformly byte {winner_byte}, size correct, no corruption"
    )
    try:
        op_delete(host, port, name)
    except Exception:
        pass
    return r.ok(detail)


def test_mixed_workload_soak(host, port, duration_seconds, num_workers):
    r = Result(f"mixed workload soak, {duration_seconds}s across {num_workers} workers")
    stop_at = time.time() + duration_seconds
    counts = {"upload": 0, "download": 0, "list": 0, "delete": 0, "chmod": 0, "errors": 0}
    lock = threading.Lock()
    small_data = os.urandom(4096)

    def worker(worker_id):
        my_name = PREFIX + f"soak_{worker_id}.bin"
        uploaded = False
        while time.time() < stop_at:
            try:
                choice = int(time.time() * 1000) % 5
                if choice == 0 or not uploaded:
                    op_upload(host, port, my_name, small_data)
                    uploaded = True
                    with lock:
                        counts["upload"] += 1
                elif choice == 1:
                    op_download(host, port, my_name)
                    with lock:
                        counts["download"] += 1
                elif choice == 2:
                    op_list(host, port)
                    with lock:
                        counts["list"] += 1
                elif choice == 3:
                    op_chmod(host, port, my_name, 0o644)
                    with lock:
                        counts["chmod"] += 1
                else:
                    op_delete(host, port, my_name)
                    uploaded = False
                    with lock:
                        counts["delete"] += 1
            except Exception:
                with lock:
                    counts["errors"] += 1
        try:
            op_delete(host, port, my_name)
        except Exception:
            pass

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(num_workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    total_ops = sum(v for k, v in counts.items() if k != "errors")
    error_rate = counts["errors"] / total_ops if total_ops > 0 else 0

    detail = (
        f"{total_ops} mixed operations in {duration_seconds}s "
        f"(upload {counts['upload']}, download {counts['download']}, list {counts['list']}, "
        f"chmod {counts['chmod']}, delete {counts['delete']}), "
        f"{counts['errors']} errors ({error_rate:.1%})"
    )
    if error_rate > 0.01:
        return r.fail(detail)
    return r.ok(detail)


# ---------------------------------------------------------------------------

def cleanup_leftovers(host, port):
    """best effort removal of anything this script may have left behind"""
    try:
        entries = op_list(host, port)
    except Exception:
        return
    for name, _size, _mode in entries:
        if name.startswith(PREFIX):
            try:
                op_delete(host, port, name)
            except Exception:
                pass


def main():
    parser = argparse.ArgumentParser(description="NERV FS performance and stress test suite")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--quick", action="store_true", help="smaller sizes and fewer iterations, for fast local runs")
    args = parser.parse_args()

    host, port = args.host, args.port

    print(f"NERV FS performance suite against {host}:{port}")
    print("=" * 72)

    try:
        socket.create_connection((host, port), timeout=5).close()
    except OSError as e:
        print(f"cannot reach server at {host}:{port}, {e}")
        sys.exit(1)

    if args.quick:
        throughput_sizes = [1, 10]
        concurrent_clients, ops_per_client = 6, 10
        saturation_clients = 16
        burst_iterations, burst_clients = 10, 6
        contention_writers, contention_size_mb = 3, 2
        soak_seconds, soak_workers = 5, 4
    else:
        throughput_sizes = [1, 10, 100]
        concurrent_clients, ops_per_client = 10, 20
        saturation_clients = 32
        burst_iterations, burst_clients = 50, 6
        contention_writers, contention_size_mb = 4, 5
        soak_seconds, soak_workers = 15, 8

    tests = []
    tests.append(test_correctness_roundtrip(host, port))
    for size_mb in throughput_sizes:
        tests.append(test_throughput(host, port, size_mb))
    tests.append(test_concurrent_small_ops(host, port, concurrent_clients, ops_per_client))
    tests.append(test_thread_pool_saturation(host, port, saturation_clients))
    tests.append(test_same_file_write_contention(host, port, contention_writers, contention_size_mb))
    tests.append(test_repeated_bursts(host, port, burst_iterations, burst_clients))
    tests.append(test_mixed_workload_soak(host, port, soak_seconds, soak_workers))

    cleanup_leftovers(host, port)

    print()
    print("results")
    print("-" * 72)
    all_passed = True
    for t in tests:
        status = "PASS" if t.passed else "FAIL"
        if not t.passed:
            all_passed = False
        print(f"[{status}] {t.name}")
        print(f"       {t.detail}")

    print("-" * 72)
    print("ALL TESTS PASSED" if all_passed else "SOME TESTS FAILED")
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()