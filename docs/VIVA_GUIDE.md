# Viva Guide: Batch Process Monitor (FCFS and Round-Robin Dispatchers)

This guide gives short, honest answers that match the code in `src/batch_monitor.c`. Where an answer names a function, that function exists in the source.

---

## Team

| Roll number | Name | Role |
|---|---|---|
| AP24110010677 | **Lakshay** | **Team Leader** |
| AP24110011219 | Md Ahab Firoz | Member |
| AP24110010386 | Akshita Modi | Member |
| AP24110011380 | N. Shanmuka Abhiram | Member |

**Who answers what in the viva.** Each member should be ready to explain their own part in depth, and everyone should know the overall flow (Part A, Q1–Q2, and the demo in Part C).

| Member | Be ready to explain | Viva questions (Part A) |
|---|---|---|
| **Lakshay** (Team Leader) | Overall architecture, the FCFS and Round-Robin loops, `fork()`/`execvp()`, budget expiry, SIGINT/SIGKILL, SIGTSTP/SIGCONT/SIGSTOP, Ctrl+C handling. Also leads the live demo. | Q1, Q2, Q7, Q8, Q9, Q12, Q15, Q17, Q19, Q22, RR1–RR8 |
| **Md Ahab Firoz** | The dispatch-file formats (including sir's commandless process list and the default `sleepy`), how a line is parsed and validated, and how commands are split into arguments. | Q5, Q16, Q18, Q20 |
| **Akshita Modi** | The PCB, the linked-list queue, `waitpid()`, reaping, zombies, and the summary metrics (waiting and turnaround time). | Q3, Q4, Q6, Q10, Q11, Q13, Q14 |
| **N. Shanmuka Abhiram** | `sleepy.c`, the test worker, the automated tests, the Makefile, valgrind, and CPU-time budget vs. real CPU use. | Q21, plus "how did you test it?" |

---

## Part A: The 22 viva questions

**1. What is a Batch Process Monitor?**
It is a program that accepts a list of jobs and runs them without user interaction, deciding *when* each one runs. Mine reads a dispatch file and creates each job as a real Linux process with `fork()` + `execvp()`. It runs the jobs one at a time in FCFS order, watches them, enforces a time budget, reaps them, and prints a report. In OS terms, it plays the role of a simple **dispatcher**.

**2. What is FCFS scheduling?**
First-Come, First-Served means the job that arrives first gets the CPU first and keeps it until it finishes. It is the simplest scheduling policy: a FIFO queue. My code only ever starts the **head** of the queue (`dispatch_next()`), so a later job can never overtake an earlier one. Jobs with the same arrival time run in file order.

- *Pro:* simple and fair in arrival order.
- *Con:* the **convoy effect**. Short jobs stuck behind a long job wait a long time. In the spec example, job 5 (2 s) waits 10 ticks.

**3. What is a Process Control Block?**
The data structure an OS keeps for each process: its identity, state and the bookkeeping needed to manage it. My `struct pcb` holds:

- the fields from the spec: `pid`, `args[]`, `arrivaltime`, `remainingcputime` and `next`;
- additions: original budget, priority/memory/resource metadata, state (READY/RUNNING/TERMINATED), start and finish ticks, real start and end timestamps, raw `waitpid` status, and flags recording *why* it ended.

**4. Why is a linked list suitable for the queue?**
A FIFO queue only needs "add at the back" and "remove from the front". With head and tail pointers, both are **O(1)** (`enqueue()`, `dequeue()`). The list grows to any number of jobs with no fixed size, and no elements need shifting the way they would in an array. It is also how the spec defines the PCB, with a `next` pointer.

**5. What does arrival time represent?**
The dispatcher-clock tick at which the job is *submitted*: the earliest moment it may start. Before then, it sits in the queue but is not eligible. If nothing has arrived yet, the CPU is idle and the program prints "CPU idle. Next job … arrives at T=n."

**6. What does `remainingcputime` represent?**
How many ticks of budget the job has left. It starts at the CPU-time field from the file. It drops by 1 at each tick while the job is running. When it reaches 0, the job is stopped with SIGINT. For simple-format lines there is no budget (`NO_BUDGET`), and the job runs until it exits by itself.

**7. What does `fork()` do?**
It creates a new process that is an almost exact copy of the caller: same code, same memory contents (copy-on-write), same open files. It returns **twice**: `0` in the child, and the child's PID in the parent (or `-1` on failure). In `launch_job()`, the `pid == 0` branch is the child and the other branch is the dispatcher.

**8. What does `execvp()` do?**
It replaces the current process image with a new program. The PID stays the same, but the code and memory become the new program's. The `v` means arguments are passed as an array (`argv`). The `p` means it searches the `PATH` if the name has no `/`. It **only returns if it fails**.

**9. Why does the child use `execvp()`?**
After `fork()`, the child is still running *the dispatcher's* code. To become the job (`sleep`, `test_worker`, …), it must load that program, and `exec` does that. `fork` + `exec` is the standard UNIX way to start a new program: `fork` makes the process, `exec` decides what it runs. Splitting them lets the child prepare itself between the two calls; mine does so with `setpgid()`, resetting signal handlers, and redirecting stdin to `/dev/null`.

**10. What does `waitpid()` do?**
It lets a parent collect a specific child's termination status. It tells the parent whether the child exited (and with what code, via `WIFEXITED`/`WEXITSTATUS`) or was killed by a signal (`WIFSIGNALED`/`WTERMSIG`). It also removes the child's entry from the kernel's process table, which is called **reaping**. I use it in two ways:

- with `WNOHANG` (do not block) every 50 ms, to check whether the child is still running (`poll_current()`);
- blocking, only when the child is certainly ending (after exec failure or `SIGKILL`).

**11. What is a zombie process?**
A process that has finished, but whose parent has not yet called `wait`/`waitpid`. Its memory is freed, but its process-table entry (PID and exit status) remains so the parent can read it. Many zombies can exhaust PIDs. My dispatcher reaps every child within about 50 ms of its exit, and the test suite checks that no `Z`-state child exists during a run.

**12. What is SIGINT?**
Signal number 2, "interrupt". It is what the terminal sends when you press Ctrl+C. Its default action is to terminate the process, but a process can **catch** it (handle it) or **ignore** it. The spec asks for SIGINT when a budget expires. Because a program can ignore SIGINT, I wait 500 ms and then send **SIGKILL** (signal 9), which cannot be caught or ignored.

**13. Why does the parent track child PIDs?**

1. `waitpid(pid)` needs the PID, to reap that exact child.
2. `kill(pid, sig)` needs it, to signal **only** our own child. Calling `kill(0, …)` or `kill(-1, …)` would signal whole groups of processes, so the code refuses any `pid <= 0`.
3. To report events and results ("PID 416 running").

**14. What happens when a process exits naturally?**
The kernel makes it a zombie and sends the parent SIGCHLD. Within about 50 ms, `poll_current()` calls `waitpid(..., WNOHANG)`, which returns the PID and status. `finish_current()` then:

- records the end time and finish tick;
- decides the reason: exit 0 is "completed", a nonzero code is "failed (exit n)", a signal is "killed by …";
- prints the event;
- calls `retire_job()`, which copies the result into the summary record and frees the PCB.

Because the CPU is now free, `wait_for_tick()` calls `dispatch_next()` immediately, so the next eligible job starts in the same tick.

**15. What happens when a job's budget expires?**
At each tick, `run_dispatcher()` decrements `remainingcputime`. When it reaches 0, it prints "CPU-time budget expired", sets `budget_expired`, and calls `terminate_current(d, SIGINT)`. That function:

1. sends `kill(pid, SIGINT)`;
2. checks for up to 500 ms whether the child has exited, reaping it if so;
3. if it is still alive, prints "sending SIGKILL (last resort)", sends `SIGKILL`, and reaps it with a blocking `waitpid`.

The summary shows "budget expired: SIGINT" or "budget expired: SIGKILL".

**16. How are future-arriving processes handled?**
All jobs are loaded into the queue at the start, sorted by arrival (the file must be in non-decreasing order, and this is checked). A job is only dispatched when it is at the head, nothing is running, **and** `arrivaltime <= clock`. If the head has not arrived yet, the CPU is idle for that tick and the dispatcher prints which job comes next and when. The arrival of each job is announced at its tick ("Job 2 arrived … Jobs ahead of it: 1").

**17. Why is FCFS non-preemptive in normal scheduling?**
In pure FCFS, the scheduler never takes the CPU away from a running job to give it to another. The next job is chosen only when the current one finishes or blocks. No time slices are used, which keeps it simple and cheap: there is only one context switch per job. In my project, the only forced stops are the safety budget (a job is *killed*, not paused and resumed later) and Ctrl+C. No job is ever paused to let another run, so the scheduling itself stays non-preemptive.

**18. What happens when an executable does not exist?**
`fork()` succeeds, but `execvp()` fails with `ENOENT` in the child. Before forking, the parent created a pipe with `FD_CLOEXEC`. On a failed exec, the child writes `errno` into the pipe and calls `_exit(127)`. The parent reads the pipe:

- **0 bytes** means exec succeeded (the pipe was closed automatically by the exec);
- **4 bytes** means it failed.

On failure, the parent reaps the child, prints "could not execute './build/no_such_program': No such file or directory", records "launch failed", and immediately tries the next eligible job. The dispatcher itself keeps running.

**19. What happens when Ctrl+C is pressed?**
The terminal sends SIGINT to the foreground process group. Each job was placed in **its own** process group with `setpgid()`, so only the dispatcher receives it. Its handler `on_stop_signal()` only stores the signal number in a `volatile sig_atomic_t` flag; doing complex work inside a handler is unsafe. The main loop notices the flag, and so does the sleep, which returns early. `shut_down()` then:

1. stops the running child (SIGINT, then SIGKILL after 500 ms if needed) and reaps it;
2. marks every queued job "not started" and frees its PCB;
3. prints the summary.

The program exits with status **130** (128 + 2), the shell convention. SIGTERM and SIGHUP are handled the same way.

**20. What are the limitations of the implementation?**

- Only one job runs at a time.
- Times are in whole 1-second ticks, so budgets are accurate to within one tick.
- The "CPU-time budget" is elapsed time, not real CPU usage.
- Priority, memory and resources are only metadata.
- Commands have no shell syntax (use `sh -c`).
- The input must already be sorted by arrival.
- `PR_SET_PDEATHSIG` is Linux-only.

**21. What is the difference between CPU-time budget and actual CPU utilization?**

- The **budget** is counted by the dispatcher: one unit per tick of *wall-clock time* during which the job holds the dispatcher's single slot.
- **CPU utilization** is how much processor time the job actually used, which the kernel measures and `getrusage()` reports.

A `sleep 3` uses 3 ticks of budget but almost **0 s** of CPU, because it is blocked the whole time. `tests/jobs_cpu_vs_wall.txt` demonstrates this: a sleeping worker and a `--busy` spinning worker each use 2 ticks, but the spinner prints `actual CPU time used: 1.98 s`. To enforce *real* CPU time, you would read `/proc/<pid>/stat` or use `setrlimit(RLIMIT_CPU)`.

**22. How does the design change for Round-Robin scheduling? (Implemented: `--rr`)**
It reuses the same PCB, queue functions, reaping and summary, and adds four things:

1. **A second queue.** The *input queue* holds jobs that have not arrived yet. The *RR queue* holds arrived jobs waiting for the CPU, both new and suspended.
2. **A quantum counter** in the PCB (`quantum_used`).
3. **Suspend.** When the quantum is used up and someone is waiting, `suspend_current()` sends `kill(pid, SIGTSTP)`, confirms the stop with `waitpid(..., WUNTRACED)`, sets the state to SUSPENDED, and appends the PCB to the tail of the RR queue.
4. **Resume.** When a suspended job reaches the head again, `resume_job()` sends `kill(pid, SIGCONT)` instead of forking again, because its PCB already has a PID.

With sir's job list and q = 1, the output matches Stallings Figure 9.5 exactly.

## Part A2: Round-Robin questions

**RR1. What is Round-Robin scheduling?**
A preemptive policy. Each ready job gets the CPU for at most one *time quantum*, then goes to the back of the queue if others are waiting. Every job gets a fair share and a quick first response. Mine uses a quantum of 1 tick by default (`--quantum N` to change it).

**RR2. Why two queues (input queue and RR queue)?**
The input queue holds the whole file in arrival order, including jobs from the "future". The RR queue holds only jobs that have *arrived* and are ready to run. Each tick, `unload_arrivals()` moves every job with `arrivaltime <= timer` from the input queue to the RR queue. Keeping them apart means a job that hasn't arrived can never be scheduled.

**RR3. What do SIGTSTP, SIGSTOP and SIGCONT do?**
- `SIGTSTP` is the "terminal stop" signal, the same one Ctrl+Z sends. Its default action **stops** (pauses) the process, but a program may catch or ignore it.
- `SIGSTOP` also stops the process, but it can **never** be caught or ignored.
- `SIGCONT` makes a stopped process continue from exactly where it was.

A stopped process keeps all its memory and state; it just isn't scheduled by the kernel. In `ps` it shows state **T**.

**RR4. How do you know the child really stopped?**
After `SIGTSTP`, the dispatcher calls `waitpid(pid, &status, WNOHANG | WUNTRACED)`. `WUNTRACED` makes `waitpid` also report *stopped* children, which `WIFSTOPPED(status)` detects. If the child has not stopped within 500 ms (it ignored SIGTSTP), the dispatcher sends `SIGSTOP` and waits for the stop.

**RR5. Why are new arrivals moved to the RR queue *before* the running job is preempted?**
Because that is the order in sir's algorithm and in Stallings' figure. A job arriving at the same tick as a preemption gets in line *ahead* of the preempted job. For example, at T=4, C arrives and is queued before B is put back, so C runs at T=5 and B at T=6. The opposite order would produce a different schedule from Figure 9.5.

**RR6. What happens if the quantum expires but no other job is waiting?**
The job keeps the CPU and its quantum counter restarts. Suspending and immediately resuming it would be wasted work. D does this at the end of the Stallings run: it runs at T=18 and T=19 without a context switch.

**RR7. Compare FCFS and RR on the same five jobs.**

| Metric | FCFS | RR, q = 1 |
|---|---|---|
| Average turnaround | 8.60 ticks | 10.80 ticks |
| Average waiting | 4.60 ticks | 6.80 ticks |

RR gives each job a quicker first response, so short or interactive jobs do not wait behind long ones (no convoy effect), but it costs more context switches (13 suspensions here). RR's waiting time is turnaround minus CPU ticks, because a job waits in several separate pieces.

**RR8. What happens to suspended jobs on Ctrl+C?**
They are real, stopped processes, so they must not be left behind. `shut_down()` stops the running job and then, for every suspended job, sends `SIGINT` followed by `SIGCONT`. The SIGCONT is needed because a stopped process cannot act on SIGINT until it runs. Then it reaps each one (with SIGKILL after 500 ms if needed). The test suite and a real terminal Ctrl+C test confirm that no process is left.

---

## Part B: Every major function, in plain English

### PCB and queue

| Function | What it does |
|---|---|
| `pcb_new()` | Allocates a zeroed PCB (`calloc`), with state READY and no budget. |
| `pcb_free(p)` | Frees every argument string, the command text, and the PCB itself. |
| `queue_init(q)` | Makes an empty queue (head = tail = NULL). |
| `enqueue(q, p)` | Adds a PCB at the tail in O(1), which keeps file order. |
| `dequeue(q)` | Removes and returns the head PCB in O(1). |
| `queue_free(q)` | Dequeues and frees everything still in the queue. |

### Parser

| Function | What it does |
|---|---|
| `trim(s)` | Removes spaces, tabs and `\r` from both ends of a string. |
| `parse_int(s, &out)` | Strict integer conversion with `strtol`. The whole string must be a number and must fit in an `int`. |
| `looks_like_int(start, end)` | Checks whether the second field is a number, which decides simple vs. full format. |
| `tokenize_command(cmd, p, …)` | Splits the command into `argv` words: on spaces, with quote grouping, at most 63 arguments, NULL-terminated for `execvp`. |
| `parse_line(raw, …)` | Turns one text line into a PCB. It skips comments and blanks, detects the format, validates every field, and reports errors with the line number. A process-list line with no command (`0, 0, 3, 64, 0, 0, 0, 0`) gets the default program `sleepy`. |
| `add_own_dir_to_path(argv0)` | Puts the dispatcher's own folder (`build/`) first in `PATH`, so a dispatch file can say `sleepy 3` exactly as on the project sheet. |
| `load_dispatch_file(path, q)` | Opens the file and reads it line by line with `getline` (no length limit). It checks arrival order, counts errors, rejects empty files, and fills the queue. On any error it frees everything and returns exit code 2. |

### Child lifecycle

| Function | What it does |
|---|---|
| `launch_job(d, p)` | Creates the exec-failure pipe, `fork()`s, and then branches. **Child:** joins its own process group, resets signals, sets stdin to `/dev/null`, calls `execvp()`, and on failure writes `errno` to the pipe. **Parent:** reads the pipe to learn whether exec worked; on success the job becomes `current`, on failure it reaps the child and records "launch failed". |
| `poll_current(d)` | Calls `waitpid(pid, WNOHANG)` on the current child. If the child has ended, it calls `finish_current()`. Retries on `EINTR`. |
| `finish_current(d, status)` | Decodes the wait status, works out the reason it ended, prints the event, and retires the job. |
| `terminate_current(d, sig)` | Sends `sig` (SIGINT) to our child, waits up to 500 ms, then sends SIGKILL if needed. Always reaps. |
| `retire_job(d, p, reason, err)` | Copies the job's results into its `JobRecord` (for the summary), then frees the PCB. |

### Dispatcher

| Function | What it does |
|---|---|
| `run_dispatcher(d)` | The main loop, i.e. the spec's algorithm. Each tick: check and decrement the running job's budget (expire it if needed), announce arrivals, dispatch if the CPU is free, then wait for the next tick and increment the clock. Stops when nothing is left, or when the stop flag is set. |
| `dispatch_next(d)` | FCFS: if nothing is running and the head of the queue has arrived, dequeues it and launches it. Repeats if a launch fails. |
| `dispatch_next_rr(d)` | RR: if nothing is running, takes the head of the RR queue. A suspended job is resumed (`resume_job`); a new one is started (`launch_job`). |
| `unload_arrivals(d)` | RR: moves every job whose arrival time has come from the input queue to the back of the RR queue. |
| `suspend_current(d)` | RR: sends SIGTSTP, confirms the stop with `waitpid(WUNTRACED)`, falls back to SIGSTOP after 500 ms, then puts the PCB at the back of the RR queue. |
| `resume_job(d, p)` | RR: sends SIGCONT and makes the job the running one again, with a fresh quantum. |
| `record_tick(d)`, `print_gantt(d)` | Record which job held the CPU in each tick, and draw the Gantt chart at the end (Stallings style). |
| `wait_for_tick(d)` | Sleeps until the next one-second boundary (an absolute monotonic deadline, so no drift), in 50 ms slices. It reaps a finished child at once and dispatches the next job at once. Returns early on Ctrl+C. |
| `announce_arrivals(d)` | Prints "Job n arrived" for jobs whose arrival time is the current tick. |
| `shut_down(d)` | The Ctrl+C path: stops and reaps the running child **and every suspended child**, and marks queued jobs "not started". |

### Output, signals and `main`

| Function | What it does |
|---|---|
| `log_event(d, fmt, …)` | Prints one event with the `[T=n]` prefix and flushes immediately, so output order is correct. |
| `print_job_table()` | Prints the jobs loaded from the file, including their metadata. |
| `print_summary()` | Prints the final table (start, finish, wait, turnaround, elapsed, result) and the averages. |
| `describe_result()` | Turns a termination reason into text such as "budget expired: SIGKILL". |
| `fit()` | Shortens a command so it fits a table column. |
| `on_stop_signal(sig)` | The signal handler. It only sets `g_stop_signal = sig`. |
| `install_signal_handlers()` | Uses `sigaction()` for SIGINT, SIGTERM and SIGHUP, without `SA_RESTART`, so sleeps wake up immediately. |
| `main()` | Handles `--help`/`--version` and usage errors, loads the file, allocates summary records, installs handlers, runs the dispatcher, prints the summary, frees everything, and returns the exit code. |

### Small helpers

| Function | What it does |
|---|---|
| `mono_now()` | Reads the monotonic clock. |
| `ts_diff_sec()`, `ts_add_ns()`, `ts_cmp()` | Arithmetic and comparison on `struct timespec`. |
| `sleep_ns_full()` | A short sleep that resumes after `EINTR`. |
| `signal_name()` | Maps a signal number to a name such as "SIGINT". |
| `parse_error()` | Prints `file:line: error: …` to stderr. |

---

## Part C: Suggested 3–5 minute live demonstration

Run everything from the repository root. Make the terminal at least 120 columns wide so the summary table does not wrap.

| Time | Do this | Say this |
|---|---|---|
| 0:00 | `make clean && make` | "A plain C11 build with every warning switched on; zero warnings." |
| 0:20 | `cat tests/jobs_sheet_processlist.txt` | "This is sir's process list exactly as given: 8 fields, no command. Each job runs `sleepy` until its CPU time runs out." |
| 0:35 | `make run` (runs `tests/jobs_sheet_processlist.txt`) | While it runs, point out arrivals while job 1 is running, "Jobs ahead of it", budget counting down, and SIGINT at expiry. At the end: "This is the textbook FCFS Gantt chart: 3, 9, 13, 18, 20. Average wait is 4.6." *(20 s)* |
| 1:20 | `./build/batch_monitor tests/jobs_budget_expiry.txt` | "Job 2 ignores SIGINT, so after 500 ms the dispatcher uses SIGKILL. That is the bounded cleanup." |
| 1:40 | `./build/batch_monitor tests/jobs_error_cases.txt` | "A missing program, exit code 3, and a crash. Each is detected and reported, and the dispatcher keeps going." |
| 2:00 | `./build/batch_monitor tests/jobs_interrupt.txt`, then press **Ctrl+C** after 3 s | "The handler only sets a flag. The main loop stops and reaps the child, job 2 is 'not started', and it exits with 130." Then run `echo $?` to show 130. |
| 2:30 | `pgrep -a test_worker; echo "none left"` | "No orphan workers are left running." |
| 2:40 | `./build/batch_monitor tests/jobs_malformed.txt` | "Every bad line is reported with its line number, and nothing runs." |
| 2:55 | `./build/batch_monitor tests/jobs_cpu_vs_wall.txt` | "Both jobs use 2 ticks of budget, but only the spinner used about 2 s of real CPU. The budget is elapsed time, not CPU usage." |
| 3:15 | `make run-rr` *(20 s)* | "Round-Robin with q = 1 on sir's list. Watch SIGTSTP suspend and SIGCONT resume. The Gantt chart at the end is identical to Stallings Figure 9.5." Optionally, in a second terminal, run `ps -o pid,stat,comm -C sleepy` to show the suspended jobs in state **T**. |
| 3:45 | `QUICK=1 make test` *(optional, 45 s)* | "30 quick automated tests with real processes; the full run has 35, including sir's three example lists exactly as given." |
| 3:50 | Open `src/batch_monitor.c` at `struct pcb`, `launch_job` and `run_dispatcher` | Walk through fork → exec → waitpid. |

**If something goes wrong in the demo:**

- "Permission denied" or "No such file" for `./build/test_worker`: you are not in the repository root, or you have not run `make` yet.
- A file has Windows line endings: this is handled automatically.

---

## Part D: Likely follow-up questions

- **What runs when a process-list line has no command?** The default program `sleepy` (`src/sleepy.c`). Started with no argument, it runs until it receives a signal. The dispatcher counts down the job's `cputime` and sends SIGINT when it reaches zero, so each job lasts exactly its CPU time, as in sir's pseudo-code.
- **Is Round-Robin implemented?** Yes: `--rr`, with `--quantum N` (default 1). With sir's job list it reproduces Stallings Figure 9.5 exactly, and `make test` checks the dispatch order tick by tick.
- **Why not run the next job immediately after SIGTSTP instead of waiting for the stop?** Then two jobs could briefly run at once. Waiting until `waitpid` confirms the stop guarantees only one job ever holds the CPU.

- **Why not do the cleanup inside the signal handler?** Only *async-signal-safe* functions may be called in a handler. `printf`, `malloc` and `free` are not, and calling them could deadlock or corrupt memory. Setting a `sig_atomic_t` flag is safe.
- **Why `CLOCK_MONOTONIC` instead of `time()`?** The monotonic clock never jumps when the system time is changed (by NTP or the user), and it gives nanosecond resolution.
- **Why absolute deadlines?** Sleeping "1 second" repeatedly adds the loop's own run time every tick, so the clock drifts. Sleeping *until* `t0 + n` does not drift.
- **Why does each job get its own process group?** So that a terminal Ctrl+C goes only to the dispatcher. The dispatcher then stops the job deliberately, records why, and reaps it. Otherwise both would receive the signal at once.
- **What is `EINTR`?** A blocking system call such as `waitpid`, `read` or `nanosleep` can return early with `errno = EINTR` when a signal arrives. The code retries those calls.
- **What if the dispatcher is killed with `kill -9`?** No program can handle SIGKILL. On Linux, `prctl(PR_SET_PDEATHSIG, SIGKILL)` in the child makes the kernel kill the job too, so no orphan keeps running.
- **What is waiting time / turnaround time?**
  - Waiting time = start − arrival.
  - Turnaround = finish − arrival.
  - Both are printed per job and averaged.
