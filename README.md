# RemoteOps — Remote System Monitoring and Management Tool over TCP/IP

IE3090 Network Programming — Assignment, Part 1
**Registration number: IT24103736**

RemoteOps has two C programs: an **Agent** (TCP server on the managed machine) and a
**Controller** (the administrator's client). Control traffic uses a fixed line-based text
protocol over TCP; periodic system statistics are streamed over UDP.

## Personalised values (all derived from IT24103736)

| Item | Formula | Value |
|---|---|---|
| Registration number | – | **IT24103736** |
| Agent listening port | 7000 + first four digits of numeric part (2410) | **9410** |
| Source files | agent_/controller_/Makefile_ + last three digits (736) | `agent_736.c`, `controller_736.c`, `Makefile_736` |
| Session ID (SID) tag | last four digits (3736) reversed | **SID:6373** |
| Authentication token | "OPS-" + last four digits | **OPS-3736** |
| Log file | remoteops_<regno>.log | **remoteops_IT24103736.log** |
| File storage path | ./agentfiles/<regno>/<filename> | **./agentfiles/IT24103736/<filename>** |
| Submission archive | IE3090_<regno>.zip | **IE3090_IT24103736.zip** |

## Build

```
make -f Makefile_736          # builds agent_736 and controller_736 (gcc, -Wall -Wextra -pthread)
make -f Makefile_736 clean
```

## Run

Terminal 1 (Agent):

```
./agent_736
```

Terminal 2 (Controller):

```
./controller_736 127.0.0.1          # optional second argument: port (default 9410)
```

Controller commands:

| Controller command | Sends | Notes |
|---|---|---|
| `auth [token]` | `AUTH <token>` | default token OPS-3736 |
| `sysinfo` | `SYSINFO` | CPU load, memory used (MB), uptime (s) |
| `listproc` | `LISTPROC` | snapshot of running processes |
| `exec <name>` | `EXEC <name>` | DATE, UPTIME, DISKFREE, HOSTNAME, WHOAMI only |
| `put <file> [remote]` | `PUT <name> <size>` + bytes | prints throughput |
| `get <remote> [local]` | `GET <name>` | saved under `./downloads/`, prints throughput |
| `monitor start <port>` | `MONITOR START <port>` | listens on that UDP port and prints datagrams |
| `monitor stop` | `MONITOR STOP` | |
| `raw <text>` | text as typed | for testing error handling |
| `quit` | `QUIT` | |

## Protocol (as specified in the brief, section 2.3)

Every command and response is one line ending in `\n`; every Agent response ends with
` SID:6373`. `PUT`/`GET` are followed by exactly `<filesize>` raw bytes.

Error codes used (the brief fixes 001, 002, 004, 005; the others are my own, self-explanatory):

| Code | Reason | When |
|---|---|---|
| 001 | AUTH_FAILED | wrong token (connection closed after 3 failures) |
| 002 | COMMAND_NOT_ALLOWED | EXEC name not on the whitelist |
| 003 | UNKNOWN_COMMAND | unrecognised command |
| 004 | FILE_TOO_LARGE | PUT larger than 10 MB |
| 005 | FILE_NOT_FOUND | GET of a file that was not uploaded |
| 006 | NOT_AUTHENTICATED | any command other than AUTH before a successful AUTH |
| 007 | BAD_ARGUMENTS | missing or malformed arguments |
| 008 | INVALID_FILENAME | file name with characters outside `A-Za-z0-9._-`, or starting with `.` |
| 009 | INTERNAL_ERROR | local failure (e.g. disk write) |
| 011 | LINE_TOO_LONG | command line longer than 4095 bytes |
| 012 | SERVER_BUSY | more than 100 simultaneous controllers |

## Design summary

* **Concurrency model:** one POSIX thread per Controller connection (`pthread_create`, detached).
  Each connection has its own state (authenticated flag, receive buffer, monitor thread), so
  there is no shared mutable state except the log file (protected by a mutex).
* **Framing:** each connection has a receive buffer. `read_line()` returns one complete line
  regardless of how many `recv()` calls it took, and keeps any extra bytes for the next call.
  `recv_body()` reads exactly `<filesize>` bytes, using buffered bytes first.
* **Safety:** EXEC input is compared against a fixed table and is never passed to a shell.
  File names are validated, so `../` path tricks cannot leave the storage directory.
  Uploads are written to a temporary file and renamed only when complete.
* **UDP monitoring:** `MONITOR START <port>` starts a per-session thread that sends
  `SYSINFO <cpu_load> <mem_used_mb> <uptime_sec> SID:6373` to the Controller's IP every 2 seconds.
  `MONITOR STOP` and `QUIT` (and any disconnect) stop it.
* **Logging:** every connection, command, response, file transfer and disconnect is written
  with a timestamp to `remoteops_IT24103736.log` (the AUTH token is never logged).
* **Optional extension implemented:** transfer throughput (bytes/second) is reported by the
  Controller for PUT and GET.

## Assumptions

* CPU load is the 1-minute load average from `/proc/loadavg`; memory used is
  MemTotal − MemAvailable from `/proc/meminfo`; uptime is from `/proc/uptime`.
* `MONITOR STOP` always replies `OK MONITOR_STOPPED`, even if monitoring was not running.
* The Agent runs on Linux; the Controller can connect to a remote Agent by giving its IP.

## Repository contents

`agent_736.c`, `controller_736.c`, `Makefile_736`, `README.md`, `DESIGN_DIARY.md`,
`tests/test_agent.py` (protocol test script).

## AI usage

AI assistance (Claude) was used for this assignment; see the prompt log submitted with the
assignment for details of what was generated, tested and changed.
