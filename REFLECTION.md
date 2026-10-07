# Reflection — RemoteOps (IT24103736)

**Which AI tools did I use, and at which stages?**
I used Claude (Anthropic) on 7 October 2026. I used it to plan the evening around the deadline, to
calculate my personalised values, to compare my starter agent with the brief, to generate the
full agent, controller and Makefile, to write a Python test script, to help me run and debug the
programs on my Ubuntu VM, and to draft the README, design diary, prompt log and report
skeleton. I wrote the first starter agent myself, before using the AI.

**What did the AI do well?**
It produced a design that matched the brief closely: one thread per controller, a buffered
`read_line()` so partial lines and several lines in one packet work, exact byte counting for
PUT/GET, a fixed EXEC whitelist, and a mutex-protected log file. Its test script checked cases I
would not have thought of, such as a command arriving in the same packet as the end of a file
upload, an 11 MB upload being rejected without breaking the connection, and a controller
disconnecting in the middle of an upload. All 43 checks passed on my own VM.

**Where did it get things wrong or mislead me?**
Some of its instructions assumed things that were not true for me. It told me to press Ctrl+L and
to type `clear` at the Controller prompt, but the Controller is not a shell, so that only
printed errors. It also first assumed I could copy and paste between Windows and my VM. One
message mixed a command with the output I should expect, and I typed both into the terminal. A
few times I had to retake screenshots because they were too small or cut off.

**What did I change, add or reject, and why?**
I replaced my own starter agent with the full implementation because it did not meet the brief.
I did not edit the generated source code itself. Instead I compiled it, ran it, and tested every
command myself on my VM, and I opened the main functions to take annotated code screenshots. I
rejected the idea of back-dating Git commits, because the brief asks for honest process
evidence. I took every screenshot from my own VM instead of using any generated output, and I
chose to add files to GitHub through the website because setting up Git on the VM was taking
too long.

**What did I learn about network programming?**
TCP is a byte stream, so one `recv()` is not one message. That is why the protocol needs a
framing layer, and why PUT and GET must count exactly `<filesize>` bytes. A server must survive
clients that vanish in the middle of a transfer. The error `bind: Address already in use` taught
me that an old process can still own a port. UDP monitoring works because the Controller listens
on its own port and the Agent sends datagrams to it. Using AI made the work much faster, but I
realised the lab test and viva need me to understand the code myself, so I will keep studying
the main functions.
