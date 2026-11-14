# Viva Cheat-Sheet: How to Explain the Source Code to Sir

Use this document to prepare for and confidently answer questions during your viva or project presentation.

---

## 1. The 30-Second Elevator Pitch
> *"Sir, our project implements an Operating System Batch Process Monitor using First-Come, First-Served (FCFS) scheduling in C.*
> *It reads a job dispatch file, stores jobs in a FIFO queue of Process Control Blocks (PCBs), and executes them as real Linux child processes using `fork()` and `execvp()`.*
> *At every 1-second clock tick, our dispatcher tracks the running process, counts down its CPU-time budget, and terminates jobs that exceed their limit using `SIGINT` (falling back to `SIGKILL` if ignored).*
> *It reaps child processes immediately using `waitpid()` to eliminate zombie processes and calculates the standard OS scheduling metrics: waiting time, turnaround time, and CPU idle time."*

---

## 2. Walkthrough of the Source Code Structure (`src/batch_monitor.c`)

The code is organized into **8 clear, modular sections**:

```text
src/batch_monitor.c
├── Section 1: PCB & Queue Data Structures
├── Section 2: FIFO Queue Operations (enqueue, dequeue)
├── Section 3: Dispatch File Parser (8-field format & simple format)
├── Section 4: Process Creation (fork + execvp)
├── Section 5: Process Monitoring & Termination (waitpid + kill)
├── Section 6: FCFS Scheduling Main Loop (1-second tick loop)
├── Section 7: Output Tables & Performance Metrics
└── Section 8: Main Function & Signal Handling
```

---

## 3. High-Frequency Viva Questions & Answers

### Q1: What is the Process Control Block (PCB) in your code?
**Where in code:** Section 1 (`struct pcb`, `typedef Pcb, *PcbPtr`)
**Answer:**
> *"The PCB is the data structure that stores all information needed by the operating system to manage a process. In `src/batch_monitor.c`, our `struct pcb` contains:*
> - `pid`: *The process identifier returned by `fork()`.*
> - `args[MAXARGS]`: *The argument vector passed to `execvp()`.*
> - `arrivaltime`: *The clock tick when the job arrives in the ready queue.*
> - `remainingcputime`: *The remaining CPU budget ticks for the job.*
> - `next`: *Pointer to the next PCB, forming our linked-list queue.*
> - *We also store `start_tick`, `finish_tick`, `cputime`, and `cmdline` to calculate scheduling metrics."*

---

### Q2: Why did you use a Linked List for the Ready Queue?
**Where in code:** Section 2 (`enqueue`, `dequeue`)
**Answer:**
> *"Sir, FCFS is a First-In, First-Out (FIFO) policy. A linked list with both a `head` and a `tail` pointer provides **$O(1)$ constant-time enqueue** at the tail and **$O(1)$ constant-time dequeue** at the head. Unlike a fixed array, it grows dynamically with no memory reallocation or shifting needed."*

---

### Q3: How do `fork()` and `execvp()` work together in your project?
**Where in code:** Section 4 (`launch_job()`)
**Answer:**
> *"When a job is dispatched:*
> 1. `fork()` *creates an exact copy of the parent process. It returns twice: `0` in the child process, and the child's `pid` in the parent process.*
> 2. *In the **child branch** (`pid == 0`), we set a new process group with `setpgid(0, 0)` so Ctrl+C only goes to the dispatcher, reset signal handlers, and call `execvp(p->args[0], p->args)`.*
> 3. `execvp()` *overwrites the child's memory image with the target executable. If `execvp()` succeeds, it never returns. If it fails (e.g. command not found), it prints an error and calls `_exit(127)`.*
> 4. *In the **parent branch**, the dispatcher stores the child's PID and transitions the PCB state to `STATE_RUNNING`."*

---

### Q4: How is the CPU-time budget enforced?
**Where in code:** Section 5 (`terminate_child()`) and Section 6 (`run_dispatcher()`)
**Answer:**
> *"Every 1-second tick, the dispatcher decrements `d->current->remainingcputime`. When it hits zero:*
> 1. *The dispatcher calls `terminate_child(d, SIGINT)`, which sends signal 2 (`SIGINT`) via `kill(p->pid, SIGINT)`.*
> 2. *It waits up to 500 milliseconds for the child to terminate cleanly.*
> 3. *If the child ignores `SIGINT`, it escalates to `SIGKILL` (signal 9), which cannot be caught or ignored, guaranteeing that runaway jobs are always stopped."*

---

### Q5: What is a Zombie Process, and how do you prevent them?
**Where in code:** Section 5 (`check_child_exit()`)
**Answer:**
> *"A zombie is a process that has completed execution, but its exit status has not yet been read by its parent, so its entry remains in the kernel process table.*
> *We prevent zombies by polling `waitpid(d->current->pid, &status, WNOHANG)` every 50ms inside the tick sleep loop and at every tick boundary. As soon as the child exits, `waitpid()` reaps it and frees its resources immediately."*

---

### Q6: How do you calculate Waiting Time and Turnaround Time?
**Where in code:** Section 7 (`print_summary()`)
**Answer:**
> *"Using standard operating system definitions:*
> - **Waiting Time** $= \text{Start Time} - \text{Arrival Time}$
> - **Turnaround Time** $= \text{Finish Time} - \text{Arrival Time}$
>
> *For our 5-job project sheet example:*
> - *Total Waiting Time $= 0 + 1 + 5 + 7 + 10 = 23$ ticks $\implies$ **Average Waiting Time: 4.60 ticks***
> - *Total Turnaround Time $= 3 + 7 + 9 + 12 + 12 = 43$ ticks $\implies$ **Average Turnaround Time: 8.60 ticks***"*

---

### Q7: What happens when no job has arrived yet (CPU idle)?
**Where in code:** Section 6 (`run_dispatcher()`)
**Answer:**
> *"If the CPU is free and the head of the queue has an `arrivaltime > d->clock`, the dispatcher increments `d->idle_ticks` and prints: `[T=n] CPU idle. Next job (Job X) arrives at T=Y.` The dispatcher sleeps for 1 second, advances the clock, and repeats until the arrival time is reached."*

---

### Q8: How does your code handle Ctrl+C safely?
**Where in code:** Section 6 and Section 8 (`handle_sigint()`)
**Answer:**
> *"The signal handler only sets a safe `volatile sig_atomic_t g_stop_signal = 1` flag. In the next iteration, the dispatcher loop catches this flag, gracefully terminates the running child process, clears remaining jobs, prints the final summary, and exits without leaving orphaned processes."*
