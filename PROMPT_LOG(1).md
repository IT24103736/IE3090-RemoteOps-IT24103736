# Prompt Log — AI interactions (IE3090 Part 1, IT24103736)

Tool used: **Claude** (Anthropic), via the Claude app, on 7 October 2026.

| # | Stage | What I asked (summary) | What the AI produced | How I used / changed it |
|---|---|---|---|---|
| 1 | Planning | Uploaded the assignment brief and asked how long the work would take. | A time estimate, a task breakdown, and reminders about the deadline, commits and academic integrity. | Used it to plan the evening. |
| 2 | Personalisation | Asked for my personalised values from IT24103736. | Port 9410, SID:6373, token OPS-3736, log, storage and file names. | Checked them against section 2.4 of the brief. |
| 3 | Review of my starter | Shared a screenshot of my project folder and pasted my starter agent code. | A list of what my starter was missing compared with the brief (commands, SID tag, concurrency, logging, Makefile). | Decided to replace the starter with a full implementation. |
| 4 | Implementation | Asked the AI to complete the program according to the brief. | `agent_736.c` (thread per connection), `controller_736.c`, `Makefile_736`, and `test_agent.py` (43 checks). | Used the code as generated (no edits to the source). Compiled it on my Ubuntu VM, ran it, and tested every command myself. |
| 5 | Debugging | Pasted terminal screenshots showing `bind: Address already in use`, and `clear` / `Ctrl+L` not working in the Controller. | Explained the causes (old agent still running; Controller is not a shell) and gave `pkill agent_736` and the shell `clear`. | Followed the steps and fixed both. |
| 6 | Evidence | Asked which screenshots the report needs and how to take them. | A checklist of screenshots and the commands to produce each one. | I ran every command and took every screenshot myself on my own VM. |
| 7 | Process evidence | Asked how to publish to GitHub without using Git commands. | Steps to create the repository and upload files with separate commit messages on the GitHub website. | Followed the steps; the commit messages are the ones suggested by the AI. |
| 8 | Documents | Asked for drafts of the README, design diary, prompt log, reflection and a report skeleton. | Draft text files and a Word skeleton with screenshot placeholders. | Read them, checked that they match what I actually did, and kept the parts that are true. The README, diary, reflection and report text were drafted with AI help. |

## Notes on responsible use

* All screenshots in my report come from my own programs running on my own VM.
* The AI-generated code is not changed by me; I will study every function so that I can explain it
  in the lab assessment and viva.
