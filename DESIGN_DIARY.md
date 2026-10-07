# Design Diary — RemoteOps (IT24103736)

## Before 7 Oct 2026 — Starter agent
I wrote a first version of `agent_736.c` on my own. It listened on port 9410, accepted **one**
controller, received **one** message, replied `OK CONNECTED` and exited. It compiled and ran,
but it had none of the commands from the brief, no SID tag, no authentication and no logging.

## 7 Oct 2026 — Reading the brief and personalising
I re-read the brief and calculated my personalised values from IT24103736:
port 7000 + 2410 = **9410**; SID 3736 reversed = **6373**; token **OPS-3736**; log file
`remoteops_IT24103736.log`; storage path `./agentfiles/IT24103736/`. I decided to keep them as
`#define` constants at the top of the agent so they are easy to check.

## 7 Oct 2026 — Design decisions
* **Concurrency:** one thread per connection. A blocking `recv()` in one thread cannot stop other
  controllers, and each connection's state (authenticated flag, receive buffer, monitor thread)
  is in one struct. `select`/`epoll` scales further but PUT/GET and the UDP monitor would need
  explicit state machines. Forked processes would make the shared log file harder.
* **Framing:** a per-connection buffer so partial lines, several lines in one `recv()` and the
  raw bytes of PUT/GET all work (`read_line()` and `recv_body()`).
* **Security:** EXEC compares the argument to a fixed table and never builds a shell command from
  user text. File names are restricted, so `../` cannot escape the storage folder.
* **Uploads:** written to a temporary file first, then renamed, so a dropped connection never
  leaves a half-written file under the real name.
* **Error codes:** the brief fixes 001, 002, 004 and 005. I added 003, 006, 007, 008, 009, 011
  and 012 (listed in the README).

## 7 Oct 2026 — Building and testing
I used AI assistance (Claude) to produce the full agent, controller and Makefile, replacing my
starter (see the prompt log). I compiled it on my Ubuntu VM, then ran the Agent and tested every
feature by hand: AUTH, SYSINFO, LISTPROC, EXEC, PUT/GET with `cmp`, the UDP monitor, error
cases, and five simultaneous controllers. I also ran the protocol test script
(`test_agent.py`): all 43 checks passed.

## 7 Oct 2026 — Obstacles
* `./agent_736` printed `bind: Address already in use`. My **old starter agent** was still
  running on port 9410. I stopped it with `pkill agent_736` and the new agent started.
* I typed `clear` inside the Controller prompt. It is not a Controller command, so it replied
  `unknown command`. I used `clear` in the normal shell instead.
* Windows and the Ubuntu VM do not share a clipboard, so I downloaded the files from inside the
  VM's browser.
* Several of my first screenshots were too small or cut off, so I retook them.
* Setting up `git` in the terminal was taking time, so I created the GitHub repository in the
  browser and added each file with its own commit message.

## What I need to study before the lab test and viva
I plan to re-read `read_line()`, `recv_body()`, `cmd_put()` and `monitor_thread()` until I can
explain them without notes, in particular: why `read_line()` keeps leftover bytes for the next
call, what happens if a Controller disconnects in the middle of a PUT, and why the Agent uses
`MSG_NOSIGNAL` and ignores SIGPIPE.
