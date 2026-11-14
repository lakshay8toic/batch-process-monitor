/*
 * batch_monitor.c - Batch Process Monitor (FCFS Process Dispatcher)
 *
 * Operating Systems Project (ID 25)
 * Team:
 *   AP24110010677  Lakshay (Team Leader)
 *   AP24110011219  Md Ahab Firoz
 *   AP24110010386  Akshita Modi
 *   AP24110011380  N. Shanmuka Abhiram
 *
 * Description:
 *   Reads batch job specifications from a file, enqueues them into a FIFO
 *   ready queue of Process Control Blocks (PCBs), and executes them in
 *   First-Come, First-Served (FCFS) order using fork(), execvp(), and waitpid().
 *   Enforces CPU-time budgets via SIGINT / SIGKILL signals.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include <stdbool.h>

#define MAXARGS         64
#define NO_BUDGET       (-1)
#define DEFAULT_PROGRAM "sleepy"
#define KILL_GRACE_MS   500

/* ========================================================================= */
/* SECTION 1: Process Control Block (PCB) & Queue Structures                 */
/* ========================================================================= */

typedef enum { STATE_READY, STATE_RUNNING, STATE_TERMINATED } ProcState;

typedef struct pcb {
    /* Required by OS Specification */
    pid_t pid;                  /* Process ID returned by fork() */
    char *args[MAXARGS];        /* Argument list for execvp() */
    int arrivaltime;            /* Dispatcher clock tick when job arrives */
    int remainingcputime;       /* Remaining CPU budget in ticks (or NO_BUDGET) */
    struct pcb *next;           /* Pointer to next PCB in linked list */

    /* Bookkeeping and Lifecycle Fields */
    int argc;
    int lineno;                 /* Line number in dispatch file */
    char *cmdline;              /* Full command string for display */
    int jobid;                  /* 1-based job identifier */
    int cputime;                /* Initial CPU budget */
    int priority;               /* Priority metadata (FCFS ignores) */
    int memory;                 /* Memory metadata (FCFS ignores) */
    int resources[4];           /* Resource metadata (FCFS ignores) */
    bool has_metadata;          /* True if 8-field format was used */
    ProcState state;            /* Current execution state */
    long start_tick;            /* Clock tick when execution started */
    long finish_tick;           /* Clock tick when process was reaped */
    int wait_status;            /* Exit status from waitpid() */
    bool budget_expired;        /* True if terminated due to CPU budget expiry */
    bool killed_by_sigkill;     /* True if SIGKILL was required */
} Pcb, *PcbPtr;

typedef struct {
    PcbPtr head;                /* Head of linked list (first to run) */
    PcbPtr tail;                /* Tail of linked list (new jobs appended here) */
    int count;
} Queue;

typedef struct {
    Queue queue;                /* Ready queue of pending jobs */
    PcbPtr current;             /* Currently running job (or NULL) */
    long clock;                 /* Dispatcher time in seconds/ticks */
    PcbPtr finished_jobs;       /* Linked list of finished jobs for final report */
    int total_jobs;
    long idle_ticks;
} Dispatcher;

static volatile sig_atomic_t g_stop_signal = 0;

/* ========================================================================= */
/* SECTION 2: Queue Management (FIFO - O(1) Enqueue & Dequeue)               */
/* ========================================================================= */

static PcbPtr pcb_new(void) {
    PcbPtr p = (PcbPtr)calloc(1, sizeof(Pcb));
    if (p) {
        p->state = STATE_READY;
        p->cputime = NO_BUDGET;
        p->remainingcputime = NO_BUDGET;
    }
    return p;
}

static void pcb_free(PcbPtr p) {
    if (!p) return;
    for (int i = 0; i < p->argc; i++) free(p->args[i]);
    free(p->cmdline);
    free(p);
}

static void enqueue(Queue *q, PcbPtr p) {
    p->next = NULL;
    if (q->tail) q->tail->next = p;
    else q->head = p;
    q->tail = p;
    q->count++;
}

static PcbPtr dequeue(Queue *q) {
    PcbPtr p = q->head;
    if (!p) return NULL;
    q->head = p->next;
    if (!q->head) q->tail = NULL;
    p->next = NULL;
    q->count--;
    return p;
}

static void append_finished(Dispatcher *d, PcbPtr p) {
    p->next = NULL;
    if (!d->finished_jobs) {
        d->finished_jobs = p;
    } else {
        PcbPtr cur = d->finished_jobs;
        while (cur->next) cur = cur->next;
        cur->next = p;
    }
}

/* ========================================================================= */
/* SECTION 3: Dispatch File Parser                                           */
/* ========================================================================= */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static int tokenize_command(const char *cmd, PcbPtr p) {
    p->argc = 0;
    const char *s = cmd;
    char word[PATH_MAX];

    while (*s) {
        while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        if (p->argc >= MAXARGS - 1) return -1;

        int len = 0;
        while (*s && !isspace((unsigned char)*s)) {
            if (*s == '\'' || *s == '"') {
                char q = *s++;
                while (*s && *s != q) word[len++] = *s++;
                if (*s == q) s++;
            } else {
                word[len++] = *s++;
            }
        }
        word[len] = '\0';
        p->args[p->argc++] = strdup(word);
    }
    p->args[p->argc] = NULL;
    return p->argc;
}

static int parse_line(char *line, int lineno, PcbPtr *out) {
    *out = NULL;
    char *trimmed = trim(line);
    if (*trimmed == '\0' || *trimmed == '#') return 0; /* Skip blank / comment */

    int fields[8] = {0};
    char *comma = strchr(trimmed, ',');
    if (!comma) {
        fprintf(stderr, "error: line %d missing comma\n", lineno);
        return -1;
    }

    /* Check if 8-field format (arrival, priority, cpu, memory, r1, r2, r3, r4) */
    char copy[PATH_MAX * 2];
    strncpy(copy, trimmed, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    int num_fields = 0;
    char *tokens[9] = {NULL};
    char *rest = copy;
    for (int i = 0; i < 8; i++) {
        char *c = strchr(rest, ',');
        if (c) {
            *c = '\0';
            tokens[i] = trim(rest);
            rest = c + 1;
            num_fields++;
        } else {
            tokens[i] = trim(rest);
            num_fields++;
            rest = NULL;
            break;
        }
    }
    if (rest) tokens[8] = trim(rest);

    bool is_eight_field = (num_fields == 8);
    for (int i = 0; i < 8 && is_eight_field; i++) {
        char *end;
        fields[i] = (int)strtol(tokens[i], &end, 10);
        if (*end != '\0') is_eight_field = false;
    }

    PcbPtr p = pcb_new();
    if (!p) return -1;
    p->lineno = lineno;

    if (is_eight_field) {
        p->has_metadata = true;
        p->arrivaltime = fields[0];
        p->priority = fields[1];
        p->cputime = fields[2];
        p->remainingcputime = fields[2];
        p->memory = fields[3];
        for (int i = 0; i < 4; i++) p->resources[i] = fields[4 + i];

        char *cmd = (tokens[8] && *tokens[8]) ? tokens[8] : DEFAULT_PROGRAM;
        p->cmdline = strdup(cmd);
        tokenize_command(cmd, p);
    } else {
        /* Simple format: <arrival>,<command> */
        char *first_comma = strchr(trimmed, ',');
        *first_comma = '\0';
        p->arrivaltime = atoi(trim(trimmed));
        char *cmd = trim(first_comma + 1);
        p->cmdline = strdup(cmd);
        tokenize_command(cmd, p);
    }

    *out = p;
    return 1;
}

static int load_dispatch_file(const char *path, Queue *q) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "error: cannot open dispatch file '%s': %s\n", path, strerror(errno));
        return 2;
    }

    char *line = NULL;
    size_t len = 0;
    int lineno = 0, prev_arrival = -1, errors = 0;

    while (getline(&line, &len, fp) != -1) {
        lineno++;
        PcbPtr p = NULL;
        int res = parse_line(line, lineno, &p);
        if (res < 0) { errors++; continue; }
        if (res == 0) continue;

        if (p->arrivaltime < prev_arrival) {
            fprintf(stderr, "%s:%d: error: arrival time %d < %d (must be non-decreasing)\n",
                    path, lineno, p->arrivaltime, prev_arrival);
            pcb_free(p);
            errors++;
            continue;
        }
        prev_arrival = p->arrivaltime;
        p->jobid = q->count + 1;
        enqueue(q, p);
    }
    free(line);
    fclose(fp);

    if (errors > 0 || q->count == 0) {
        if (q->count == 0 && errors == 0)
            fprintf(stderr, "error: '%s' contains no jobs\n", path);
        return 2;
    }
    return 0;
}

/* ========================================================================= */
/* SECTION 4: Process Creation & Execution (fork + execvp)                  */
/* ========================================================================= */

static void add_own_dir_to_path(const char *argv0) {
    char dir[PATH_MAX] = {0};
    const char *slash = strrchr(argv0, '/');
    if (slash) {
        size_t len = (size_t)(slash - argv0);
        if (len >= sizeof(dir)) len = sizeof(dir) - 1;
        strncpy(dir, argv0, len);
        dir[len] = '\0';
    } else {
        strcpy(dir, ".");
    }

    char resolved[PATH_MAX];
    char *abs = realpath(dir, resolved);
    if (!abs) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(resolved, sizeof(resolved), "%s/%s", cwd, dir);
            abs = resolved;
        } else {
            abs = dir;
        }
    }

    const char *old = getenv("PATH");
    char new_path[PATH_MAX * 2];
    snprintf(new_path, sizeof(new_path), "%s:%s", abs, old ? old : "/bin:/usr/bin");
    setenv("PATH", new_path, 1);
}

static bool launch_job(Dispatcher *d, PcbPtr p) {
    p->start_tick = d->clock;
    printf("[T=%ld] Starting Job %d: %s\n", d->clock, p->jobid, p->cmdline);
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[T=%ld] ERROR: fork() failed for Job %d: %s\n", d->clock, p->jobid, strerror(errno));
        p->state = STATE_TERMINATED;
        p->finish_tick = d->clock;
        append_finished(d, p);
        return false;
    }

    if (pid == 0) {
        /* Child Process */
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        execvp(p->args[0], p->args);
        /* If execvp returns, error occurred */
        fprintf(stderr, "[T=%ld] ERROR: could not execute '%s': %s\n", d->clock, p->args[0], strerror(errno));
        _exit(127);
    }

    /* Parent Process */
    p->pid = pid;
    p->state = STATE_RUNNING;
    d->current = p;

    if (p->remainingcputime == NO_BUDGET)
        printf("[T=%ld] Child launched with PID %d (no CPU budget: runs until exit).\n", d->clock, (int)pid);
    else
        printf("[T=%ld] Child launched with PID %d. CPU-time budget: %d tick(s).\n", d->clock, (int)pid, p->remainingcputime);
    fflush(stdout);

    return true;
}

/* ========================================================================= */
/* SECTION 5: Process Monitoring & Termination (waitpid, kill)              */
/* ========================================================================= */

static bool check_child_exit(Dispatcher *d) {
    if (!d->current) return false;
    int status;
    pid_t res = waitpid(d->current->pid, &status, WNOHANG);

    if (res == d->current->pid) {
        PcbPtr p = d->current;
        p->state = STATE_TERMINATED;
        p->finish_tick = d->clock;
        p->wait_status = status;

        if (WIFEXITED(status))
            printf("[T=%ld] Child reaped: PID %d exited with status %d.\n", d->clock, (int)p->pid, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            printf("[T=%ld] Child reaped: PID %d killed by signal %d.\n", d->clock, (int)p->pid, WTERMSIG(status));

        d->current = NULL;
        append_finished(d, p);
        return true;
    }
    return false;
}

static void terminate_child(Dispatcher *d, int sig) {
    if (!d->current || d->current->pid <= 0) return;
    PcbPtr p = d->current;

    printf("[T=%ld] Sending signal %d to PID %d (budget expired).\n", d->clock, sig, (int)p->pid);
    kill(p->pid, sig);

    /* Wait up to 500ms for clean termination */
    for (int i = 0; i < 10; i++) {
        usleep(50000); /* 50 ms */
        if (check_child_exit(d)) return;
    }

    /* Fallback to SIGKILL if child ignored SIGINT */
    printf("[T=%ld] PID %d did not terminate; sending SIGKILL.\n", d->clock, (int)p->pid);
    p->killed_by_sigkill = true;
    kill(p->pid, SIGKILL);
    waitpid(p->pid, &p->wait_status, 0);

    p->state = STATE_TERMINATED;
    p->finish_tick = d->clock;
    d->current = NULL;
    append_finished(d, p);
}

/* ========================================================================= */
/* SECTION 6: FCFS Scheduling Loop                                          */
/* ========================================================================= */

static void dispatch_next(Dispatcher *d) {
    while (!d->current && d->queue.head && d->queue.head->arrivaltime <= d->clock) {
        PcbPtr p = dequeue(&d->queue);
        printf("[T=%ld] Job %d is eligible (arrived at T=%d). Launching FCFS.\n",
               d->clock, p->jobid, p->arrivaltime);
        launch_job(d, p);
    }
}

static void run_dispatcher(Dispatcher *d) {
    d->clock = 0;
    printf("[T=0] Dispatcher clock initialized to 0.\n");

    while (d->queue.head || d->current) {
        if (g_stop_signal) {
            printf("\n[T=%ld] Interrupted by user. Shutting down...\n", d->clock);
            if (d->current) terminate_child(d, SIGINT);
            while (d->queue.head) {
                PcbPtr p = dequeue(&d->queue);
                p->state = STATE_TERMINATED;
                append_finished(d, p);
            }
            break;
        }

        /* 1. Announce arrivals at this clock tick */
        for (PcbPtr p = d->queue.head; p; p = p->next) {
            if (p->arrivaltime == d->clock)
                printf("[T=%ld] Job %d arrived (%s).\n", d->clock, p->jobid, p->cmdline);
        }

        /* 2. Dispatch if CPU is free */
        dispatch_next(d);

        /* 3. Status check */
        if (d->current) {
            if (d->current->remainingcputime != NO_BUDGET) {
                printf("[T=%ld] PID %d running. Remaining budget: %d tick(s).\n",
                       d->clock, (int)d->current->pid, d->current->remainingcputime);
            } else {
                printf("[T=%ld] PID %d running (no budget limit).\n", d->clock, (int)d->current->pid);
            }
        } else {
            if (!d->queue.head) break;
            printf("[T=%ld] CPU idle. Next job (Job %d) arrives at T=%d.\n",
                   d->clock, d->queue.head->jobid, d->queue.head->arrivaltime);
            d->idle_ticks++;
        }

        /* 4. Advance time by 1 tick (1000 ms), polling for early natural exit */
        bool early_exit = false;
        for (int i = 0; i < 20; i++) {
            usleep(50000); /* 50ms intervals = 1000ms */
            if (g_stop_signal) break;
            if (d->current && check_child_exit(d)) {
                early_exit = true;
                break;
            }
        }

        d->clock++;

        /* 5. Process management after 1 second of CPU time */
        if (d->current && !early_exit) {
            if (check_child_exit(d)) {
                /* Exited right at the tick boundary */
            } else if (d->current->remainingcputime != NO_BUDGET) {
                d->current->remainingcputime--;
                if (d->current->remainingcputime <= 0) {
                    printf("[T=%ld] CPU-time budget expired for PID %d.\n", d->clock, (int)d->current->pid);
                    d->current->budget_expired = true;
                    terminate_child(d, SIGINT);
                }
            }
        }
    }

    printf("[T=%ld] All jobs finished. Dispatcher exiting.\n", d->clock);
}

/* ========================================================================= */
/* SECTION 7: Output Tables & Performance Metrics                            */
/* ========================================================================= */

static void print_banner(void) {
    printf("=============================================\n");
    printf("        BATCH PROCESS MONITOR\n");
    printf("        FCFS PROCESS DISPATCHER\n");
    printf("=============================================\n\n");
}

static void print_job_table(const Queue *q, const char *path) {
    printf("Loaded %d job(s) from %s.\n\n", q->count, path);
    printf("JOB  ARRIVAL  BUDGET  COMMAND\n");
    printf("---  -------  ------  ----------------------------\n");
    for (PcbPtr p = q->head; p; p = p->next) {
        if (p->cputime == NO_BUDGET)
            printf("%3d  %7d  %6s  %s\n", p->jobid, p->arrivaltime, "none", p->cmdline);
        else
            printf("%3d  %7d  %6d  %s\n", p->jobid, p->arrivaltime, p->cputime, p->cmdline);
    }
    printf("\n");
}

static void print_summary(const Dispatcher *d) {
    long total_wait = 0, total_turn = 0;
    int started_count = 0;

    printf("\n====================================================================================\n");
    printf("FINAL EXECUTION SUMMARY (Times in Dispatcher Ticks / Seconds)\n");
    printf("====================================================================================\n");
    printf("JOB  PID      COMMAND                  ARRIVE  BUDGET  START  FINISH  WAIT  TURN  RESULT\n");
    printf("---  -------  -----------------------  ------  ------  -----  ------  ----  ----  -----------------------\n");

    for (PcbPtr p = d->finished_jobs; p; p = p->next) {
        char budget_str[16];
        if (p->cputime == NO_BUDGET) strcpy(budget_str, "none");
        else snprintf(budget_str, sizeof(budget_str), "%d", p->cputime);

        long wait = p->start_tick - p->arrivaltime;
        long turn = p->finish_tick - p->arrivaltime;
        if (wait < 0) wait = 0;
        if (turn < 0) turn = 0;

        char result[40];
        if (p->budget_expired) {
            snprintf(result, sizeof(result), p->killed_by_sigkill ? "budget expired: SIGKILL" : "budget expired: SIGINT");
        } else if (WIFEXITED(p->wait_status)) {
            int code = WEXITSTATUS(p->wait_status);
            snprintf(result, sizeof(result), code == 0 ? "completed (exit 0)" : "failed (exit %d)", code);
        } else if (WIFSIGNALED(p->wait_status)) {
            snprintf(result, sizeof(result), "killed by signal %d", WTERMSIG(p->wait_status));
        } else {
            snprintf(result, sizeof(result), "not started");
        }

        printf("%3d  %-7d  %-23.23s  %6d  %6s  %5ld  %6ld  %4ld  %4ld  %s\n",
               p->jobid, (int)p->pid, p->cmdline, p->arrivaltime, budget_str,
               p->start_tick, p->finish_tick, wait, turn, result);

        total_wait += wait;
        total_turn += turn;
        started_count++;
    }

    if (started_count > 0) {
        printf("\nTotal Jobs: %d | Avg Waiting Time: %.2f ticks | Avg Turnaround Time: %.2f ticks\n",
               started_count, (double)total_wait / started_count, (double)total_turn / started_count);
    }
    printf("Total Dispatcher Runtime: %ld tick(s), CPU idle: %ld tick(s).\n\n", d->clock, d->idle_ticks);
}

/* ========================================================================= */
/* SECTION 8: Main Function                                                  */
/* ========================================================================= */

static void handle_sigint(int sig) {
    (void)sig;
    g_stop_signal = 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dispatch-file>\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("Usage: %s <dispatch-file>\n", argv[0]);
        printf("FCFS Batch Process Monitor with CPU Budgeting.\n");
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("batch_monitor 1.0.0 (FCFS Process Dispatcher)\n");
        return 0;
    }

    signal(SIGINT, handle_sigint);
    add_own_dir_to_path(argv[0]);

    Dispatcher d;
    memset(&d, 0, sizeof(d));

    print_banner();
    if (load_dispatch_file(argv[1], &d.queue) != 0) return 2;

    d.total_jobs = d.queue.count;
    print_job_table(&d.queue, argv[1]);
    run_dispatcher(&d);
    print_summary(&d);

    /* Clean up allocated memory */
    PcbPtr cur = d.finished_jobs;
    while (cur) {
        PcbPtr next = cur->next;
        pcb_free(cur);
        cur = next;
    }

    return 0;
}
