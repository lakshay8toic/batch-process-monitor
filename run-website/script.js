/**
 * Batch Process Monitor - Interactive FCFS Dispatcher Simulator
 * Faithfully mirrors the C implementation logic (fork, execvp, ticks, budgets, SIGINT)
 */

// Presets matching the C project's test and benchmark suites
const PRESETS = {
  sheet_demo: `# Benchmark from the Operating Systems Course Project Sheet
# <arrival>, <priority>, <cputime>, <memory>, <res1>..<res4>
0, 0, 3, 64, 0, 0, 0, 0
2, 0, 6, 64, 0, 0, 0, 0
4, 0, 4, 64, 0, 0, 0, 0
6, 0, 5, 64, 0, 0, 0, 0
8, 0, 2, 64, 0, 0, 0, 0`,

  quick_demo: `# Quick 3-Job demo with custom commands
0, sleepy 2
1, sleepy 3
2, echo "Job 3 completed"`,

  simple_demo: `# Simple sequence format
0, sleepy 3
3, sleepy 2
5, sleepy 1`
};

const JOB_COLORS = [
  '#06b6d4', // Cyan
  '#3b82f6', // Blue
  '#10b981', // Green
  '#f59e0b', // Yellow
  '#ec4899', // Pink
  '#8b5cf6', // Purple
  '#14b8a6', // Teal
  '#f97316'  // Orange
];

class DispatcherSimulator {
  constructor() {
    this.rawJobs = [];
    this.readyQueue = [];
    this.currentJob = null;
    this.completedJobs = [];
    this.clock = 0;
    this.isRunning = false;
    this.timerId = null;
    this.speed = 1.0;
    this.timeline = []; // Recording per second for Gantt
    this.nextPid = 1001;

    this.initDOM();
    this.bindEvents();
    this.loadPreset('sheet_demo');
  }

  initDOM() {
    this.dom = {
      presetSelector: document.getElementById('preset-selector'),
      dispatchInput: document.getElementById('dispatch-input'),
      btnLoad: document.getElementById('btn-load'),
      btnRun: document.getElementById('btn-run'),
      btnStep: document.getElementById('btn-step'),
      btnPause: document.getElementById('btn-pause'),
      btnReset: document.getElementById('btn-reset'),
      speedRange: document.getElementById('speed-range'),
      speedVal: document.getElementById('speed-val'),
      clockDisplay: document.getElementById('clock-display'),
      cpuStatusBadge: document.getElementById('cpu-status-badge'),
      activePidDisplay: document.getElementById('active-pid-display'),
      completedCount: document.getElementById('completed-count'),
      currentJobTitle: document.getElementById('current-job-title'),
      currentJobDesc: document.getElementById('current-job-desc'),
      budgetProgressContainer: document.getElementById('budget-progress-container'),
      budgetProgressBar: document.getElementById('budget-progress-bar'),
      pcbPid: document.getElementById('pcb-pid'),
      pcbCmd: document.getElementById('pcb-cmd'),
      pcbArrival: document.getElementById('pcb-arrival'),
      pcbRemCpu: document.getElementById('pcb-rem-cpu'),
      pcbPrio: document.getElementById('pcb-prio'),
      pcbMem: document.getElementById('pcb-mem'),
      readyQueueItems: document.getElementById('ready-queue-items'),
      queueCount: document.getElementById('queue-count'),
      ganttTrack: document.getElementById('gantt-track'),
      ganttScale: document.getElementById('gantt-scale'),
      terminalOutput: document.getElementById('terminal-output'),
      btnClearLog: document.getElementById('btn-clear-log'),
      summaryTbody: document.getElementById('summary-tbody'),
      metricsSummary: document.getElementById('metrics-summary'),
      avgWait: document.getElementById('avg-wait'),
      avgTat: document.getElementById('avg-tat'),
      totalTime: document.getElementById('total-time'),
      cpuUtil: document.getElementById('cpu-util'),
    };
  }

  bindEvents() {
    this.dom.presetSelector.addEventListener('change', (e) => {
      if (e.target.value !== 'custom') {
        this.loadPreset(e.target.value);
      }
    });

    this.dom.btnLoad.addEventListener('click', () => this.handleLoadJobs());
    this.dom.btnRun.addEventListener('click', () => this.startRunning());
    this.dom.btnPause.addEventListener('click', () => this.pauseRunning());
    this.dom.btnStep.addEventListener('click', () => this.tick());
    this.dom.btnReset.addEventListener('click', () => this.resetSimulation());
    this.dom.btnClearLog.addEventListener('click', () => {
      this.dom.terminalOutput.innerHTML = '';
    });

    this.dom.speedRange.addEventListener('input', (e) => {
      this.speed = parseFloat(e.target.value);
      this.dom.speedVal.textContent = `${this.speed.toFixed(1)}x`;
      if (this.isRunning) {
        this.pauseRunning();
        this.startRunning();
      }
    });
  }

  loadPreset(presetKey) {
    if (PRESETS[presetKey]) {
      this.dom.dispatchInput.value = PRESETS[presetKey];
    }
  }

  log(msg, type = 'info') {
    const el = document.createElement('div');
    el.className = `log-line ${type}`;
    el.textContent = msg;
    this.dom.terminalOutput.appendChild(el);
    this.dom.terminalOutput.scrollTop = this.dom.terminalOutput.scrollHeight;
  }

  parseInput(text) {
    const lines = text.split('\n');
    const jobs = [];
    let lineNum = 0;
    let jobIdx = 1;

    for (const rawLine of lines) {
      lineNum++;
      const line = rawLine.trim();
      if (!line || line.startsWith('#')) continue;

      const parts = line.split(',').map(s => s.trim());
      if (parts.length === 0) continue;

      const arrival = parseInt(parts[0], 10);
      if (isNaN(arrival) || arrival < 0) {
        throw new Error(`Line ${lineNum}: Invalid arrival time '${parts[0]}'`);
      }

      // Check format
      if (parts.length >= 8) {
        // Sheet full format: arrival, priority, cputime, memory, ...
        const prio = parseInt(parts[1], 10) || 0;
        const burst = parseInt(parts[2], 10) || 1;
        const mem = parseInt(parts[3], 10) || 64;
        jobs.push({
          id: jobIdx,
          name: `Job ${jobIdx}`,
          arrival: arrival,
          priority: prio,
          burst: burst,
          remainingCpu: burst,
          memory: mem,
          command: `sleepy ${burst}`,
          color: JOB_COLORS[(jobIdx - 1) % JOB_COLORS.length],
          pid: null,
          startTime: null,
          finishTime: null,
          status: 'PENDING'
        });
      } else if (parts.length >= 2) {
        // Simple format: arrival, command
        const cmd = parts[1];
        let burst = 2; // default
        const match = cmd.match(/\d+/);
        if (match) {
          burst = parseInt(match[0], 10);
        }
        jobs.push({
          id: jobIdx,
          name: `Job ${jobIdx}`,
          arrival: arrival,
          priority: 0,
          burst: burst,
          remainingCpu: burst,
          memory: 32,
          command: cmd,
          color: JOB_COLORS[(jobIdx - 1) % JOB_COLORS.length],
          pid: null,
          startTime: null,
          finishTime: null,
          status: 'PENDING'
        });
      } else {
        throw new Error(`Line ${lineNum}: Unrecognized dispatch line format`);
      }
      jobIdx++;
    }

    if (jobs.length === 0) {
      throw new Error('No valid jobs found in dispatch input.');
    }

    // Verify chronological order
    for (let i = 1; i < jobs.length; i++) {
      if (jobs[i].arrival < jobs[i - 1].arrival) {
        throw new Error(`Arrival times must not decrease: Job ${jobs[i].id} (${jobs[i].arrival}) < Job ${jobs[i - 1].id} (${jobs[i - 1].arrival})`);
      }
    }

    return jobs;
  }

  handleLoadJobs() {
    try {
      this.resetSimulation();
      this.rawJobs = this.parseInput(this.dom.dispatchInput.value);
      this.log(`[DISPATCHER] Successfully loaded ${this.rawJobs.length} jobs into dispatcher pool.`, 'dispatch');
      
      this.dom.btnRun.disabled = false;
      this.dom.btnStep.disabled = false;
      this.dom.completedCount.textContent = `0 / ${this.rawJobs.length}`;
      this.render();
    } catch (err) {
      this.log(`[ERROR] ${err.message}`, 'error');
    }
  }

  startRunning() {
    if (this.isRunning) return;
    this.isRunning = true;
    this.dom.btnRun.disabled = true;
    this.dom.btnPause.disabled = false;
    this.dom.btnStep.disabled = true;

    const interval = 1000 / this.speed;
    this.timerId = setInterval(() => this.tick(), interval);
  }

  pauseRunning() {
    this.isRunning = false;
    if (this.timerId) {
      clearInterval(this.timerId);
      this.timerId = null;
    }
    this.dom.btnRun.disabled = false;
    this.dom.btnPause.disabled = true;
    this.dom.btnStep.disabled = false;
  }

  resetSimulation() {
    this.pauseRunning();
    this.clock = 0;
    this.readyQueue = [];
    this.currentJob = null;
    this.completedJobs = [];
    this.timeline = [];
    this.nextPid = 1001;

    this.dom.btnRun.disabled = true;
    this.dom.btnStep.disabled = true;
    this.dom.btnPause.disabled = true;

    this.dom.metricsSummary.style.display = 'none';
    this.dom.summaryTbody.innerHTML = '<tr><td colspan="10" class="text-center text-muted">No completed jobs yet.</td></tr>';
    this.dom.ganttTrack.innerHTML = '';
    this.dom.ganttScale.innerHTML = '';

    this.render();
    this.log(`[SYSTEM] Simulation reset. Clock initialized to T=0s.`, 'system');
  }

  /**
   * One discrete 1-second dispatcher cycle
   * Implements the exact C dispatcher logic:
   * 1. Check if jobs arrived at current clock and enqueue
   * 2. If current running job budget reaches zero -> reap & retire
   * 3. If CPU idle and ready queue has job -> dispatch_next() (fork/execvp)
   * 4. Advance clock
   */
  tick() {
    const currentClock = this.clock;

    // 1. Enqueue newly arrived jobs
    const arrived = this.rawJobs.filter(j => j.arrival === currentClock && j.status === 'PENDING');
    for (const job of arrived) {
      job.status = 'READY';
      this.readyQueue.push(job);
      this.log(`[T=${currentClock}] Job ${job.id} (${job.command}) arrived -> Enqueued to Ready Queue.`, 'info');
    }

    // 2. Manage currently running job budget
    if (this.currentJob) {
      this.currentJob.remainingCpu--;
      
      if (this.currentJob.remainingCpu <= 0) {
        // Process CPU time budget exhausted -> send SIGINT
        this.currentJob.finishTime = currentClock;
        this.currentJob.status = 'COMPLETED (SIGINT)';
        this.completedJobs.push(this.currentJob);

        this.log(`[T=${currentClock}] [PID ${this.currentJob.pid}] CPU time budget exhausted -> SIGINT sent -> reaped (exit 0).`, 'exit');
        this.currentJob = null;
      }
    }

    // 3. If CPU idle, dispatch next from ready queue (FCFS)
    if (!this.currentJob && this.readyQueue.length > 0) {
      const nextJob = this.readyQueue.shift();
      nextJob.pid = this.nextPid++;
      nextJob.startTime = currentClock;
      nextJob.status = 'RUNNING';
      this.currentJob = nextJob;

      this.log(`[T=${currentClock}] Dispatching Job ${nextJob.id} (fork -> PID ${nextJob.pid}, execvp "${nextJob.command}", budget: ${nextJob.burst}s)`, 'dispatch');
    }

    // Record timeline for Gantt chart
    if (this.currentJob) {
      this.timeline.push({
        second: currentClock,
        jobId: this.currentJob.id,
        name: this.currentJob.name,
        color: this.currentJob.color
      });
    } else {
      this.timeline.push({
        second: currentClock,
        jobId: null,
        name: 'IDLE',
        color: '#1e293b'
      });
    }

    // Advance clock
    this.clock++;

    // Check if entire batch is complete
    const allDone = this.rawJobs.every(j => j.status.startsWith('COMPLETED')) && !this.currentJob && this.readyQueue.length === 0;

    this.render();

    if (allDone) {
      this.log(`[T=${this.clock}] *** ALL JOBS TERMINATED. Dispatcher shutdown complete. ***`, 'dispatch');
      this.pauseRunning();
      this.dom.btnRun.disabled = true;
      this.dom.btnStep.disabled = true;
      this.computeMetrics();
    }
  }

  render() {
    this.dom.clockDisplay.textContent = `T = ${this.clock}s`;
    this.dom.completedCount.textContent = `${this.completedJobs.length} / ${this.rawJobs.length}`;

    // CPU Stage
    if (this.currentJob) {
      this.dom.cpuStatusBadge.textContent = 'RUNNING';
      this.dom.cpuStatusBadge.className = 'stat-value status-running';
      this.dom.activePidDisplay.textContent = `PID ${this.currentJob.pid}`;

      this.dom.currentJobTitle.textContent = `${this.currentJob.name} (${this.currentJob.command})`;
      this.dom.currentJobDesc.textContent = `Running on CPU. Budget left: ${this.currentJob.remainingCpu}s of ${this.currentJob.burst}s`;
      
      this.dom.budgetProgressContainer.style.display = 'block';
      const pct = (this.currentJob.remainingCpu / this.currentJob.burst) * 100;
      this.dom.budgetProgressBar.style.width = `${Math.max(0, pct)}%`;

      this.dom.pcbPid.textContent = this.currentJob.pid;
      this.dom.pcbCmd.textContent = this.currentJob.command;
      this.dom.pcbArrival.textContent = `${this.currentJob.arrival}s`;
      this.dom.pcbRemCpu.textContent = `${this.currentJob.remainingCpu}s`;
      this.dom.pcbPrio.textContent = this.currentJob.priority;
      this.dom.pcbMem.textContent = `${this.currentJob.memory} MB`;
    } else {
      this.dom.cpuStatusBadge.textContent = 'IDLE';
      this.dom.cpuStatusBadge.className = 'stat-value status-idle';
      this.dom.activePidDisplay.textContent = 'None';

      this.dom.currentJobTitle.textContent = 'CPU Idle';
      this.dom.currentJobDesc.textContent = this.readyQueue.length > 0 ? 'Scheduling next job...' : 'Waiting for job arrival...';
      this.dom.budgetProgressContainer.style.display = 'none';

      this.dom.pcbPid.textContent = '-';
      this.dom.pcbCmd.textContent = '-';
      this.dom.pcbArrival.textContent = '-';
      this.dom.pcbRemCpu.textContent = '-';
      this.dom.pcbPrio.textContent = '-';
      this.dom.pcbMem.textContent = '-';
    }

    // Ready Queue display
    this.dom.queueCount.textContent = `${this.readyQueue.length} waiting`;
    if (this.readyQueue.length === 0) {
      this.dom.readyQueueItems.innerHTML = '<div class="empty-state">Queue is currently empty.</div>';
    } else {
      this.dom.readyQueueItems.innerHTML = this.readyQueue.map((job, idx) => `
        <div class="queue-node ${idx === 0 ? 'head-node' : ''}">
          <div class="node-title">
            <span>${job.name}</span>
            ${idx === 0 ? '<span style="font-size:0.65rem; color:#06b6d4;">HEAD</span>' : ''}
          </div>
          <div class="node-meta">
            Cmd: ${job.command}<br>
            Arrival: T=${job.arrival}s<br>
            Burst: ${job.burst}s
          </div>
        </div>
      `).join('');
    }

    // Gantt Chart rendering
    this.renderGantt();

    // Summary Table
    this.renderSummaryTable();
  }

  renderGantt() {
    this.dom.ganttTrack.innerHTML = this.timeline.map((item) => `
      <div class="gantt-block ${item.jobId ? '' : 'idle'}" style="background-color: ${item.color}" title="T=${item.second}s: ${item.name}">
        ${item.jobId ? `J${item.jobId}` : '·'}
      </div>
    `).join('');
  }

  renderSummaryTable() {
    if (this.completedJobs.length === 0) return;

    this.dom.summaryTbody.innerHTML = this.completedJobs.map(job => {
      const waitTime = job.startTime - job.arrival;
      const turnAroundTime = job.finishTime - job.arrival;
      return `
        <tr>
          <td><strong>${job.name}</strong></td>
          <td><code>${job.pid}</code></td>
          <td><code>${job.command}</code></td>
          <td>${job.arrival}s</td>
          <td>${job.burst}s</td>
          <td>${job.startTime}s</td>
          <td>${job.finishTime}s</td>
          <td>${waitTime}s</td>
          <td>${turnAroundTime}s</td>
          <td><span class="badge-status sigint">${job.status}</span></td>
        </tr>
      `;
    }).join('');
  }

  computeMetrics() {
    if (this.completedJobs.length === 0) return;

    let totalWait = 0;
    let totalTat = 0;
    let totalBurst = 0;

    for (const job of this.completedJobs) {
      const wait = job.startTime - job.arrival;
      const tat = job.finishTime - job.arrival;
      totalWait += wait;
      totalTat += tat;
      totalBurst += job.burst;
    }

    const n = this.completedJobs.length;
    const avgWait = (totalWait / n).toFixed(2);
    const avgTat = (totalTat / n).toFixed(2);
    const totalTime = this.clock;
    const cpuUtil = ((totalBurst / totalTime) * 100).toFixed(1);

    this.dom.avgWait.textContent = `${avgWait}s`;
    this.dom.avgTat.textContent = `${avgTat}s`;
    this.dom.totalTime.textContent = `${totalTime}s`;
    this.dom.cpuUtil.textContent = `${cpuUtil}%`;

    this.dom.metricsSummary.style.display = 'grid';
  }
}

// Instantiate on DOM load
window.addEventListener('DOMContentLoaded', () => {
  new DispatcherSimulator();
});
