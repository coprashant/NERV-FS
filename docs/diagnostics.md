# Diagnostics and Profiling

Three tools, one full request cycle each. All captures were taken against
the actual nervfs_server binary on loopback, port varies per run.

## strace, syscall sequence per request

Command used:

```
strace -f -e trace=network,file -o strace_output.txt ./nervfs_server 9200
```

Then from another terminal: upload a small file, list, download it back,
then Ctrl+C the server. Relevant lines from the capture, one thread id per
column:

```
1028  socket(AF_INET, SOCK_STREAM, IPPROTO_IP) = 3
1028  setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
1028  bind(3, {sa_family=AF_INET, sin_port=htons(9200), sin_addr=inet_addr("0.0.0.0")}, 16) = 0
1028  listen(3, 16)                     = 0
1028  accept(3, {sa_family=AF_INET, sin_port=htons(53266), sin_addr=inet_addr("127.0.0.1")}, [16]) = 4
1029  openat(AT_FDCWD, ".../storage/strace_test.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644) = 5
1028  accept(3, {sa_family=AF_INET, sin_port=htons(53268), ...}, [16]) = 4
1030  openat(AT_FDCWD, ".../storage", O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY) = 5
1030  newfstatat(AT_FDCWD, ".../storage/strace_test.txt", {st_mode=S_IFREG|0644, st_size=13, ...}, 0) = 0
1030  newfstatat(AT_FDCWD, ".../storage/.gitkeep", {st_mode=S_IFREG|0644, st_size=0, ...}, 0) = 0
1028  accept(3, {sa_family=AF_INET, sin_port=htons(53284), ...}, [16]) = 4
1031  openat(AT_FDCWD, ".../storage/strace_test.txt", O_RDONLY) = 5
1028  --- SIGINT {si_signo=SIGINT, si_code=SI_USER, si_pid=1044, si_uid=0} ---
1033  +++ exited with 0 +++
1034  +++ exited with 0 +++
1029  +++ exited with 0 +++
1030  +++ exited with 0 +++
1031  +++ exited with 0 +++
1032  +++ exited with 0 +++
1035  +++ exited with 0 +++
1036  +++ exited with 0 +++
1028  +++ exited with 0 +++
```

What this shows: the main thread (1028) does the classic
socket/setsockopt/bind/listen sequence once, then one accept() per
connection since every request in this protocol is a fresh connection.
Each accepted client is handed to a worker thread (1029, 1030, 1031, ...)
that does the actual file syscall for its opcode: O_WRONLY|O_CREAT|O_TRUNC
for the upload, an O_DIRECTORY open plus one fstatat per entry for the
list, plain O_RDONLY for the download. The `-f` flag is what makes the
worker threads show up as separate lines here. When SIGINT was sent, every
thread including the 8 pool workers exits with 0, which is the graceful
shutdown from Phase 8 actually draining and joining cleanly, not just the
process getting torn down by the default SIGINT action.

## ss and netstat, concurrent connections on one port

Four Python clients opened connections and held them open for a few
seconds while the server was running, no requests sent yet, just to
freeze the ESTABLISHED state for the snapshot:

```
$ ss -tnp
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port Process
ESTAB  0      0      127.0.0.1:9201      127.0.0.1:41836   users:(("nervfs_server",pid=1061,fd=5))
ESTAB  0      0      127.0.0.1:41832     127.0.0.1:9201    users:(("python3",pid=1071,fd=3))
ESTAB  0      0      127.0.0.1:41836     127.0.0.1:9201    users:(("python3",pid=1071,fd=4))
ESTAB  0      0      127.0.0.1:41838     127.0.0.1:9201    users:(("python3",pid=1071,fd=5))
ESTAB  0      0      127.0.0.1:41854     127.0.0.1:9201    users:(("python3",pid=1071,fd=6))
ESTAB  0      0      127.0.0.1:9201      127.0.0.1:41854   users:(("nervfs_server",pid=1061,fd=7))
ESTAB  0      0      127.0.0.1:9201      127.0.0.1:41838   users:(("nervfs_server",pid=1061,fd=6))
ESTAB  0      0      127.0.0.1:9201      127.0.0.1:41832   users:(("nervfs_server",pid=1061,fd=4))
```

`netstat -tnp` shows the identical picture with the older column layout.
The point worth calling out: there is exactly one process, pid 1061, one
listening socket on port 9201, and four simultaneous ESTABLISHED
connections against it on fds 4, 5, 6, 7. This is the select loop plus
thread pool architecture from Phases 2 and 4 doing its job, one process
serving many clients concurrently rather than forking a process or
spinning up a listener per client.

## tcpdump and Wireshark, raw header bytes on the wire

Command used:

```
tcpdump -i lo -U -w capture.pcap port 9203
```

`-U` forces tcpdump to flush each packet to disk immediately, otherwise
libpcap can buffer several packets before the write actually happens,
which matters if you plan to stop the capture quickly.

One upload of a 24 byte text file to a 14 character filename produced 12
packets: SYN, SYN-ACK, ACK, then a 16 byte request header, a 48 byte
request payload, a 16 byte response header, then the FIN teardown. Hex
dump of the request header packet:

```
0x0030:  eedc 072b ae01 0001 0000 0030 0000 0000
0x0040:  0000 0000
```

Reading `ae01 0001 0000 0030 0000 0000 0000 0000` as the 16 byte
nervfs_header_t: `ae` is the magic byte, `01` is the protocol version,
`0001` is the opcode (OP_REQ_UPLOAD), `0000 0030` is payload_len as a
big endian uint32 which is 48 in decimal and matches the very next
packet's length exactly, and the trailing 8 zero bytes are the reserved
field. The response header two packets later reads
`ae01 0080 0000 0000 0000 0000 0000 0000`, same magic and version,
opcode `0080` (OP_RESP_OK), payload_len 0 since a bare acknowledgement
carries no body. Opening the same capture.pcap in Wireshark and following
the TCP stream gives the identical bytes with the added benefit of the
stream reassembly view.