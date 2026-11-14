# Step-by-Step Terminal Execution Guide

This guide explains how to compile, provide input to, and run the **Batch Process Monitor (FCFS Dispatcher)** on the terminal, with a detailed breakdown of input formats and output results.

---

## 1. Quick Start (1-Minute Run)

To compile and run a 5-second live test right now:

```bash
# 1. Compile the project
make

# 2. Run the quick demo
./build/batch_monitor RUN/INPUTS/input_quick_demo.txt
```

Or run the interactive menu:
```bash
bash RUN/demo.sh
```

---

## 2. Compilation Instructions

The project uses standard POSIX C (C11) and requires `gcc` and `make`.

### Method A: Using `make` (Recommended)
From the repository root:
```bash
make clean
make
```
This builds three executables in the `build/` folder:
- `build/batch_monitor` : The main FCFS batch dispatcher
- `build/sleepy`        : The standard simulated workload program
- `build/test_worker`   : A helper program for custom timing and test cases

### Method B: Direct Compilation with `gcc`
If `make` is not available:
```bash
mkdir -p build
gcc -std=c11 -Wall -Wextra -O2 -g -o build/batch_monitor src/batch_monitor.c
gcc -std=c11 -Wall -Wextra -O2 -g -o build/sleepy src/sleepy.c
```

---

## 3. Input File Formats

The dispatcher reads jobs from plain text files. Blank lines and lines starting with `#` are ignored. Jobs must be listed in **non-decreasing order of arrival time**.

The program accepts **two formats**:

### Format 1: 8-Field Specification Format (With CPU Budget)
```text
<arrival>,<priority>,<cputime>,<memory>,<r1>,<r2>,<r3>,<r4>[,<command>]
```
- `<arrival>` : Clock tick when job arrives at the ready queue (e.g. 0, 2, 4).
- `<cputime>` : Maximum CPU time budget allocated to the job in ticks.
- `<priority>, <memory>, <r1..r4>` : Process metadata recorded in the PCB (stored for future scheduling policies, ignored by basic FCFS).
- `[,<command>]` : Optional command. If omitted, it automatically defaults to `sleepy`. When the CPU time runs out, the dispatcher sends `SIGINT` to terminate the process.

**Example (`RUN/INPUTS/input_sheet.txt`):**
```text
0, 0, 3, 64, 0, 0, 0, 0
2, 0, 6, 64, 0, 0, 0, 0
4, 0, 4, 64, 0, 0, 0, 0
6, 0, 5, 64, 0, 0, 0, 0
8, 0, 2, 64, 0, 0, 0, 0
```

---

### Format 2: Simple Command Format (Runs Until Exit)
```text
<arrival>,<command and arguments>
```
Runs any standard terminal/Linux command. It has no artificial time budget and runs until natural completion.

**Example (`RUN/INPUTS/input_simple.txt`):**
```text
0,echo "Starting batch job 1"
1,sleep 2
4,echo "Batch process monitor finished all jobs successfully!"
```

---

## 4. How to Run with Input & Inspect Output

### Example A: Running the Fast 5-Second Demo
**Command:**
```bash
./build/batch_monitor RUN/INPUTS/input_quick_demo.txt
```

**Input Provided (`RUN/INPUTS/input_quick_demo.txt`):**
```text
0, 0, 2, 64, 0, 0, 0, 0
1, 0, 3, 64, 0, 0, 0, 0
```

**Terminal Output:**
```text
=============================================
        BATCH PROCESS MONITOR
        FCFS PROCESS DISPATCHER
=============================================

Loaded 2 job(s) from RUN/INPUTS/input_quick_demo.txt.

JOB  ARRIVAL  BUDGET  COMMAND
---  -------  ------  ----------------------------
  1        0       2  sleepy
  2        1       3  sleepy

[T=0] Dispatcher clock initialized to 0.
[T=0] Job 1 arrived (sleepy).
[T=0] Job 1 is eligible (arrived at T=0). Launching FCFS.
[T=0] Starting Job 1: sleepy
[T=0] Child launched with PID 35085. CPU-time budget: 2 tick(s).
    [sleepy pid=35085] started: running until stopped
    [sleepy pid=35085] tick 1
    [sleepy pid=35085] tick 2
[T=0] PID 35085 running. Remaining budget: 2 tick(s).
[T=1] Job 2 arrived (sleepy).
[T=1] PID 35085 running. Remaining budget: 1 tick(s).
[T=2] CPU-time budget expired for PID 35085.
[T=2] Sending signal 2 to PID 35085 (budget expired).
[T=2] Child reaped: PID 35085 killed by signal 2.
[T=2] Job 2 is eligible (arrived at T=1). Launching FCFS.
[T=2] Starting Job 2: sleepy
[T=2] Child launched with PID 35088. CPU-time budget: 3 tick(s).
    [sleepy pid=35088] started: running until stopped
    [sleepy pid=35088] tick 1
    [sleepy pid=35088] tick 2
    [sleepy pid=35088] tick 3
[T=2] PID 35088 running. Remaining budget: 3 tick(s).
[T=3] PID 35088 running. Remaining budget: 2 tick(s).
[T=4] PID 35088 running. Remaining budget: 1 tick(s).
[T=5] CPU-time budget expired for PID 35088.
[T=5] Sending signal 2 to PID 35088 (budget expired).
[T=5] Child reaped: PID 35088 killed by signal 2.
[T=5] All jobs finished. Dispatcher exiting.

====================================================================================
FINAL EXECUTION SUMMARY (Times in Dispatcher Ticks / Seconds)
====================================================================================
JOB  PID      COMMAND                  ARRIVE  BUDGET  START  FINISH  WAIT  TURN  RESULT
---  -------  -----------------------  ------  ------  -----  ------  ----  ----  -----------------------
  1  35085    sleepy                        0       2      0       2     0     2  budget expired: SIGINT
  2  35088    sleepy                        1       3      2       5     1     4  budget expired: SIGINT

Total Jobs: 2 | Avg Waiting Time: 0.50 ticks | Avg Turnaround Time: 3.00 ticks
Total Dispatcher Runtime: 5 tick(s), CPU idle: 0 tick(s).
```

---

### Example B: Running the Full 20-Second Specification Test
**Command:**
```bash
./build/batch_monitor RUN/INPUTS/input_sheet.txt
# or simply:
make run
```

**Results Breakdown:**
- **Job 1**: Arrived $T=0$, ran $T=0 \to 3$ (Wait: 0, Turnaround: 3)
- **Job 2**: Arrived $T=2$, waited until $T=3$, ran $T=3 \to 9$ (Wait: 1, Turnaround: 7)
- **Job 3**: Arrived $T=4$, waited until $T=9$, ran $T=9 \to 13$ (Wait: 5, Turnaround: 9)
- **Job 4**: Arrived $T=6$, waited until $T=13$, ran $T=13 \to 18$ (Wait: 7, Turnaround: 12)
- **Job 5**: Arrived $T=8$, waited until $T=18$, ran $T=18 \to 20$ (Wait: 10, Turnaround: 12)

**Final Metrics:**
$$\text{Average Waiting Time} = \frac{0 + 1 + 5 + 7 + 10}{5} = 4.60 \text{ ticks}$$
$$\text{Average Turnaround Time} = \frac{3 + 7 + 9 + 12 + 12}{5} = 8.60 \text{ ticks}$$

---

## 5. How to Create and Run a Custom Input File

You can create any custom text file with your own jobs:

1. Create a file, e.g. `my_jobs.txt`:
```bash
cat << 'EOF' > my_jobs.txt
0,echo "Task A started"
1,sleep 3
2,echo "Task C finished"
EOF
```

2. Run the batch monitor:
```bash
./build/batch_monitor my_jobs.txt
```
