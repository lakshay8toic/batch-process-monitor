# Run Website: Batch Process Monitor Live Web Simulator

This directory contains the complete interactive web application and simulator for the **Batch Process Monitor (FCFS Process Dispatcher)**.

---

## 🌐 Live Project Website

Click the link below to open and use the live deployed website:

👉 **[Launch Batch Process Monitor Web App](https://lakshay8toic.github.io/batch-process-monitor/)**

*(Local preview: [Open index.html](file:///Users/lakshay/.gemini/antigravity/scratch/batch-process-monitor/run-website/index.html) or run `python3 -m http.server 8000` inside this folder)*

---

## ✨ Features of the Web Simulator

1. **Interactive FCFS Dispatcher Loop:**
   - Visualizes exact discrete 1-second ticks mirroring `wait_for_tick()` in the C implementation.
   - Enqueues newly arrived jobs dynamically at their arrival times (`arrivaltime <= clock`).
   - Dispatches jobs using linked-list ready queue order (Head → Tail).
   - Simulates process budget countdowns and process termination with `SIGINT` and `waitpid()` reaping.

2. **Full Visual Instrumentation:**
   - **Dispatcher Clock:** Live `T = X` seconds display.
   - **Active Process Control Block (PCB):** Real-time monitoring of active PID, command, remaining CPU budget, priority, and allocated memory.
   - **Linked-List Ready Queue:** Visual representation of queued jobs with Head badge.
   - **CPU Execution Timeline (Gantt Chart):** Live color-coded per-second blocks representing running jobs and idle periods.
   - **Terminal Console Output:** Step-by-step logs formatted identically to the C monitor stdout.
   - **Metrics & Summary Table:** Computes exact metrics upon completion:
     - Average Waiting Time (AWT)
     - Average Turnaround Time (ATT)
     - Total Execution Span & CPU Utilization

3. **Standard Presets Included:**
   - **Sheet Benchmark (5 Jobs):** Produces the exact textbook benchmark schedule from the OS project specification (Finishes at 3, 9, 13, 18, 20 with AWT = 4.60s and ATT = 8.60s).
   - **Quick 3-Job Demo:** Fast demonstration sequence.
   - **Simple Sequence:** Sequential job arrival test.
   - **Custom Input:** Test your own dispatch files directly in the browser.

---

## 🚀 Running Locally

To run this website locally on your computer:

```bash
cd run-website
python3 -m http.server 8000
```
Then visit: [http://localhost:8000](http://localhost:8000)
