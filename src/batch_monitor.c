/*
 * batch_monitor.c - Batch Process Monitor: an FCFS process dispatcher
 *
 * Operating Systems end-semester project (ID 25).
 *
 * Team:
 *   AP24110010677  Lakshay (Team Leader)
 *   AP24110011219  Md Ahab Firoz
 *   AP24110010386  Akshita Modi
 *   AP24110011380  N. Shanmuka Abhiram
 *
 * The dispatcher reads a list of jobs from a "dispatch file", keeps them in a
 * linked-list queue of Process Control Blocks (PCBs), and runs them with
 * fork(), execvp() and waitpid() using one of two scheduling policies:
 *
 *   FCFS (default)  First-Come, First-Served: each job runs until it finishes.
 *   RR   (--rr)     Round-Robin with a time quantum (default 1 tick): when the
 *                   quantum is used up and another job is waiting, the running
 *                   job is suspended with SIGTSTP, put at the back of the RR
 *                   queue, and later resumed with SIGCONT.
 *
 * A job whose CPU-time budget runs out is sent SIGINT (with a bounded SIGKILL
 * fallback). Every child is reaped, so no zombies or orphans are left behind.
 *
 * Build:  gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -o batch_monitor batch_monitor.c
 * Run:    ./batch_monitor <dispatch-file>                 (FCFS)
 *         ./batch_monitor --rr [--quantum N] <dispatch-file>  (Round-Robin)
 *
 * This file is self-contained: it needs only the C standard library and POSIX.
 */

#define _POSIX_C_SOURCE 200809L /* getline, clock_gettime, sigaction, ... */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h> /* PR_SET_PDEATHSIG: Linux-only safety net */
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                 */
/* ------------------------------------------------------------------------ */

#define PROGRAM_NAME    "batch_monitor"
#define PROGRAM_VERSION "1.1.0"

#define MAXARGS         64          /* max argv entries per job, incl. NULL  */
#define METADATA_FIELDS 8           /* arrival,priority,cpu,mem,res1..res4    */
#define NO_BUDGET       (-1)        /* simple format: no CPU-time budget      */
#define DEFAULT_PROGRAM "sleepy"    /* run for 8-field lines with no command  */

#define NS_PER_SEC      1000000000L
#define TICK_NS         NS_PER_SEC  /* one dispatcher tick = 1 second         */
#define POLL_NS         50000000L   /* check for child exit every 50 ms       */
#define KILL_GRACE_MS   500         /* wait this long after SIGINT, then KILL */
#define STOP_GRACE_MS   500         /* wait this long after SIGTSTP, then STOP */
#define STOP_POLL_NS    10000000L   /* check for "stopped" every 10 ms        */
#define GANTT_MAX_TICKS 120         /* widest Gantt chart printed             */

/* Exit codes of the dispatcher itself. */
#define EXIT_OK      0  /* dispatcher ran every job (job failures are in the summary) */
#define EXIT_USAGE   1  /* bad command-line arguments                                */
#define EXIT_INPUT   2  /* dispatch file missing, unreadable, empty or malformed    */
#define EXIT_SYSTEM  3  /* out of memory or a system call failed fatally           */
/* Interrupted by a signal: 128 + signal number (130 for Ctrl+C), as shells do. */

/* ------------------------------------------------------------------------ */
/* Process Control Block                                                     */
/* ------------------------------------------------------------------------ */

typedef enum {
    STATE_READY,      /* loaded, waiting in the dispatch queue */
    STATE_RUNNING,    /* forked and exec'd, currently managed  */
    STATE_SUSPENDED,  /* RR only: stopped with SIGTSTP, queued */
    STATE_TERMINATED  /* reaped (or never started)             */
} ProcState;

typedef enum {
    REASON_NONE,
    REASON_COMPLETED,      /* exited with status 0 before its budget ran out  */
    REASON_EXIT_FAILURE,   /* exited by itself with a nonzero status          */
    REASON_BUDGET_SIGINT,  /* budget expired, SIGINT ended it                 */
    REASON_BUDGET_SIGKILL, /* budget expired, ignored SIGINT, SIGKILL used    */
    REASON_SIGNALED,       /* killed by some other signal (crash, external)   */
    REASON_EXEC_FAILED,    /* execvp() failed (e.g. program not found)        */
    REASON_FORK_FAILED,    /* fork() or pipe() failed                         */
    REASON_INTERRUPTED,    /* dispatcher was interrupted while it ran         */
    REASON_NOT_STARTED     /* dispatcher was interrupted before it started    */
} TermReason;

struct pcb {
    /* --- fields from the assignment's PCB --- */
    pid_t pid;                 /* child PID once launched, 0 before            */
    char *args[MAXARGS];       /* argv for execvp(), NULL-terminated, malloc'd */
    int arrivaltime;           /* dispatcher tick at which the job arrives     */
    int remainingcputime;      /* ticks of budget left, or NO_BUDGET           */
    struct pcb *next;          /* next PCB in the linked-list queue            */

    /* --- extensions for lifecycle management --- */
    int argc;                  /* number of entries used in args[]            */
    char *cmdline;             /* original command text, for display         */
    int jobid;                 /* 1-based position in the dispatch file      */
    int lineno;                /* source line in the dispatch file           */
    int cputime;               /* original budget (NO_BUDGET if none)        */
    bool has_metadata;         /* true if the 8-field format was used        */
    int priority;              /* metadata only: NOT used by FCFS            */
    int memory;                /* metadata only: NOT used by FCFS            */
    int resources[4];          /* metadata only: NOT used by FCFS            */
    ProcState state;
    long starttick;            /* dispatcher clock when launched             */
    long finishtick;           /* dispatcher clock when reaped               */
    struct timespec start_mono;/* real (monotonic) launch time               */
    struct timespec end_mono;  /* real (monotonic) reap time                 */
    int waitstatus;            /* raw status from waitpid()                  */
    bool budget_expired;       /* dispatcher sent SIGINT for budget expiry   */
    bool used_sigkill;         /* child ignored SIGINT; SIGKILL was needed   */
    bool interrupted;          /* stopped because the dispatcher was stopped */
    int quantum_used;          /* RR: ticks used in the current time slice   */
    long run_ticks;            /* ticks this job has held the CPU            */
    int suspensions;           /* RR: how many times it was suspended        */
    bool used_sigstop;         /* RR: ignored SIGTSTP, so SIGSTOP was used   */
};

typedef struct pcb Pcb;
typedef Pcb *PcbPtr;

/* FIFO queue implemented as a singly linked list with head and tail. */
typedef struct {
    PcbPtr head;   /* next job to dispatch (earliest arrival) */
    PcbPtr tail;   /* last job, where new jobs are appended   */
    int count;
} Queue;

/* What is kept about a job after its PCB has been freed (for the summary). */
typedef struct {
    int jobid;
    char *command;
    int arrival;
    int budget;
    pid_t pid;
    long start;
    long finish;
    double elapsed;        /* observed wall-clock seconds, start to reap */
    int waitstatus;
    int exec_errno;
    TermReason reason;
    bool started;
    long run_ticks;        /* ticks on the CPU                         */
    int suspensions;       /* RR: times suspended with SIGTSTP/SIGSTOP */
} JobRecord;

/* Everything the dispatcher owns, in one place. */
typedef struct {
    Queue queue;            /* input queue: jobs not yet arrived (RR) or not yet started (FCFS) */
    Queue rrqueue;          /* RR only: arrived jobs waiting for the CPU (new or suspended)     */
    bool rr;                /* true = Round-Robin, false = FCFS   */
    int quantum;            /* RR time quantum, in ticks          */
    PcbPtr current;         /* the one running job, or NULL       */
    long clock;             /* dispatcher timer, in ticks         */
    struct timespec t0;     /* monotonic time at clock = 0        */
    struct timespec next_tick;
    JobRecord *records;     /* one record per job, for summary    */
    int njobs;
    long idle_ticks;        /* ticks with no job running          */
    int *timeline;          /* job id that held the CPU in each tick (0 = idle), for the Gantt chart */
    long timeline_len;
    long timeline_cap;
} Dispatcher;

/* Set by the signal handler; read by the main loop. */
static volatile sig_atomic_t g_stop_signal = 0;

#ifdef __GNUC__
#define PRINTF_LIKE(a, b) __attribute__((format(printf, a, b)))
#else
#define PRINTF_LIKE(a, b)
#endif

/* ------------------------------------------------------------------------ */
/* Small helpers: time, logging, signal names                                */
/* ------------------------------------------------------------------------ */

static struct timespec mono_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

static double ts_diff_sec(struct timespec from, struct timespec to)
{
    return (double)(to.tv_sec - from.tv_sec) +
           (double)(to.tv_nsec - from.tv_nsec) / (double)NS_PER_SEC;
}

static struct timespec ts_add_ns(struct timespec t, long ns)
{
    t.tv_nsec += ns;
    while (t.tv_nsec >= NS_PER_SEC) {
        t.tv_nsec -= NS_PER_SEC;
        t.tv_sec++;
    }
    return t;
}

static int ts_cmp(struct timespec a, struct timespec b)
{
    if (a.tv_sec != b.tv_sec)
        return a.tv_sec < b.tv_sec ? -1 : 1;
    if (a.tv_nsec != b.tv_nsec)
        return a.tv_nsec < b.tv_nsec ? -1 : 1;
    return 0;
}

/* Sleep for ns nanoseconds, resuming after EINTR (used only for short waits). */
static void sleep_ns_full(long ns)
{
    struct timespec req = { ns / NS_PER_SEC, ns % NS_PER_SEC }, rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
        req = rem;
}

/* Print one scheduling event, prefixed with the dispatcher clock. */
static void PRINTF_LIKE(2, 3) log_event(const Dispatcher *d, const char *fmt, ...)
{
    va_list ap;
    printf("[T=%ld] ", d->clock);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGHUP:  return "SIGHUP";
    case SIGINT:  return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGILL:  return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    case SIGKILL: return "SIGKILL";
    case SIGSEGV: return "SIGSEGV";
    case SIGPIPE: return "SIGPIPE";
    case SIGALRM: return "SIGALRM";
    case SIGTERM: return "SIGTERM";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
    default:      return "unknown signal";
    }
}

/* ------------------------------------------------------------------------ */
/* PCB and queue management                                                  */
/* ------------------------------------------------------------------------ */

static PcbPtr pcb_new(void)
{
    PcbPtr p = calloc(1, sizeof *p); /* zeroes every field, args[] = NULL */
    if (p) {
        p->state = STATE_READY;
        p->cputime = NO_BUDGET;
        p->remainingcputime = NO_BUDGET;
    }
    return p;
}

static void pcb_free(PcbPtr p)
{
    if (!p)
        return;
    for (int i = 0; i < p->argc; i++)
        free(p->args[i]);
    free(p->cmdline);
    free(p);
}

static void queue_init(Queue *q)
{
    q->head = q->tail = NULL;
    q->count = 0;
}

/* Append to the tail: preserves file order, so equal arrivals stay FCFS. */
static void enqueue(Queue *q, PcbPtr p)
{
    p->next = NULL;
    if (q->tail)
        q->tail->next = p;
    else
        q->head = p;
    q->tail = p;
    q->count++;
}

/* Remove from the head: the job that came first is served first. */
static PcbPtr dequeue(Queue *q)
{
    PcbPtr p = q->head;
    if (!p)
        return NULL;
    q->head = p->next;
    if (!q->head)
        q->tail = NULL;
    p->next = NULL;
    q->count--;
    return p;
}

static void queue_free(Queue *q)
{
    PcbPtr p;
    while ((p = dequeue(q)) != NULL)
        pcb_free(p);
}

/* ------------------------------------------------------------------------ */
/* Dispatch-file parser                                                      */
/* ------------------------------------------------------------------------ */

typedef enum { PARSE_OK, PARSE_SKIP, PARSE_ERROR, PARSE_NOMEM } ParseResult;

static const char *const FIELD_NAMES[METADATA_FIELDS] = {
    "arrival time", "priority", "CPU time", "memory allocation",
    "resource 1 allocation", "resource 2 allocation",
    "resource 3 allocation", "resource 4 allocation"
};

static void PRINTF_LIKE(3, 4)
parse_error(const char *file, int lineno, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", file, lineno);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Trim leading and trailing whitespace in place; returns the new start. */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

/* Strict integer parse: the whole (already trimmed) string must be a number. */
static bool parse_int(const char *s, int *out)
{
    char *end;
    long v;

    if (*s == '\0')
        return false;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || v < INT_MIN || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}

/* True if the text in [start, end) is an integer, ignoring spaces around it.
 * Used only to decide which of the two file formats a line is written in. */
static bool looks_like_int(const char *start, const char *end)
{
    while (start < end && isspace((unsigned char)*start))
        start++;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    if (start < end && (*start == '+' || *start == '-'))
        start++;
    if (start == end)
        return false;
    for (; start < end; start++)
        if (!isdigit((unsigned char)*start))
            return false;
    return true;
}

/*
 * Split a command string into argv words for execvp().
 *  - Words are separated by spaces or tabs.
 *  - Single or double quotes group words: --name "two words" is one argument.
 *    The quote characters themselves are removed.
 *  - No shell features: no pipes, redirection, variables, globbing or escapes.
 * Returns the number of words, -1 on a syntax error, -2 if out of memory.
 */
static int tokenize_command(const char *cmd, PcbPtr p, const char *file, int lineno)
{
    size_t len = strlen(cmd);
    const char *s = cmd;

    p->argc = 0;
    for (;;) {
        while (isspace((unsigned char)*s))
            s++;
        if (*s == '\0')
            break;
        if (p->argc >= MAXARGS - 1) {
            parse_error(file, lineno, "command has more than %d arguments", MAXARGS - 1);
            return -1;
        }
        char *word = malloc(len + 1); /* a word can never be longer than cmd */
        if (!word)
            return -2;
        size_t n = 0;
        while (*s != '\0' && !isspace((unsigned char)*s)) {
            if (*s == '\'' || *s == '"') {
                char quote = *s++;
                while (*s != '\0' && *s != quote)
                    word[n++] = *s++;
                if (*s != quote) {
                    free(word);
                    parse_error(file, lineno, "unterminated %c quote in command", quote);
                    return -1;
                }
                s++; /* skip closing quote */
            } else {
                word[n++] = *s++;
            }
        }
        word[n] = '\0';
        p->args[p->argc++] = word;
    }
    p->args[p->argc] = NULL; /* execvp() needs a NULL-terminated argv */

    if (p->argc == 0 || p->args[0][0] == '\0') {
        parse_error(file, lineno, "empty program name in command");
        return -1;
    }
    return p->argc;
}

/*
 * Parse one line of the dispatch file. Two formats are accepted:
 *
 *   simple:  <arrival>,<command and arguments>
 *   full:    <arrival>,<priority>,<cpu>,<memory>,<r1>,<r2>,<r3>,<r4>[,<command>]
 *
 * Rule: if the field after the first comma is an integer, the line is the
 * full format and must have all 8 numeric fields. Everything after the 8th
 * comma (or the 1st, in simple format) is the command, so the command itself
 * may contain commas. A full-format line with no command (exactly the
 * process-list format from the project sheet, e.g. "0, 0, 3, 64, 0, 0, 0, 0")
 * runs the default program DEFAULT_PROGRAM ("sleepy"), which keeps running
 * until the dispatcher stops it when its CPU-time budget expires.
 */
static ParseResult parse_line(char *raw, const char *file, int lineno, PcbPtr *out)
{
    char *line = trim(raw);
    char *fields[METADATA_FIELDS];
    char *command;
    int values[METADATA_FIELDS] = { 0 };
    bool full;

    *out = NULL;
    if (*line == '\0' || *line == '#')
        return PARSE_SKIP;

    char *comma = strchr(line, ',');
    if (!comma) {
        parse_error(file, lineno,
                    "missing ',': expected '<arrival>,<command>' or "
                    "'<arrival>,<priority>,<cpu>,<mem>,<r1>,<r2>,<r3>,<r4>,<command>'");
        return PARSE_ERROR;
    }
    *comma = '\0';
    fields[0] = line;
    char *rest = comma + 1;

    char *next = strchr(rest, ',');
    full = looks_like_int(rest, next ? next : rest + strlen(rest));

    if (full) {
        char *cursor = rest;
        for (int i = 1; i < METADATA_FIELDS; i++) {
            char *c = strchr(cursor, ',');
            if (!c) {
                if (i + 1 == METADATA_FIELDS) {
                    fields[i] = cursor; /* 8th field, and no command follows */
                    cursor = NULL;
                    break;
                }
                parse_error(file, lineno,
                            "metadata format needs 8 numeric fields (optionally followed "
                            "by a command), but only %d field(s) were found", i + 1);
                return PARSE_ERROR;
            }
            *c = '\0';
            fields[i] = cursor;
            cursor = c + 1;
        }
        command = cursor ? trim(cursor) : (char *)"";
        if (*command == '\0')
            command = (char *)DEFAULT_PROGRAM; /* project-sheet format: no command */
        for (int i = 0; i < METADATA_FIELDS; i++) {
            char *f = trim(fields[i]);
            if (!parse_int(f, &values[i])) {
                parse_error(file, lineno, "%s '%s' is not a valid integer", FIELD_NAMES[i], f);
                return PARSE_ERROR;
            }
            if (values[i] < 0) {
                parse_error(file, lineno, "%s must not be negative (got %d)", FIELD_NAMES[i], values[i]);
                return PARSE_ERROR;
            }
        }
        if (values[2] < 1) {
            parse_error(file, lineno, "CPU time must be at least 1 tick (got %d)", values[2]);
            return PARSE_ERROR;
        }
    } else {
        char *f = trim(fields[0]);
        if (!parse_int(f, &values[0])) {
            parse_error(file, lineno, "arrival time '%s' is not a valid integer", f);
            return PARSE_ERROR;
        }
        if (values[0] < 0) {
            parse_error(file, lineno, "arrival time must not be negative (got %d)", values[0]);
            return PARSE_ERROR;
        }
        command = trim(rest);
    }

    if (*command == '\0') {
        parse_error(file, lineno, "missing command after the last comma");
        return PARSE_ERROR;
    }

    PcbPtr p = pcb_new();
    if (!p)
        return PARSE_NOMEM;
    p->lineno = lineno;
    p->arrivaltime = values[0];
    p->has_metadata = full;
    if (full) {
        p->priority = values[1];
        p->cputime = values[2];
        p->remainingcputime = values[2];
        p->memory = values[3];
        for (int i = 0; i < 4; i++)
            p->resources[i] = values[4 + i];
    }
    p->cmdline = strdup(command);
    if (!p->cmdline) {
        pcb_free(p);
        return PARSE_NOMEM;
    }
    int rc = tokenize_command(command, p, file, lineno);
    if (rc < 0) {
        pcb_free(p);
        return rc == -2 ? PARSE_NOMEM : PARSE_ERROR;
    }
    *out = p;
    return PARSE_OK;
}

/*
 * Read the whole dispatch file into the queue. Every line is checked, so the
 * user sees all mistakes at once. Returns EXIT_OK or an error exit code; on
 * error the queue is left empty.
 */
static int load_dispatch_file(const char *path, Queue *q)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "error: cannot open dispatch file '%s': %s\n", path, strerror(errno));
        return EXIT_INPUT;
    }

    char *buf = NULL;      /* getline() grows this as needed: no fixed limit */
    size_t cap = 0;
    ssize_t n;
    int lineno = 0, errors = 0, prev_arrival = -1, prev_line = 0;
    int result = EXIT_OK;

    while ((n = getline(&buf, &cap, fp)) != -1) {
        lineno++;
        if ((size_t)n != strlen(buf)) {
            parse_error(path, lineno, "line contains a NUL byte (is this a text file?)");
            errors++;
            continue;
        }
        PcbPtr p;
        ParseResult r = parse_line(buf, path, lineno, &p);
        if (r == PARSE_SKIP)
            continue;
        if (r == PARSE_NOMEM) {
            fprintf(stderr, "error: out of memory while reading '%s'\n", path);
            result = EXIT_SYSTEM;
            break;
        }
        if (r == PARSE_ERROR) {
            errors++;
            continue;
        }
        if (p->arrivaltime < prev_arrival) {
            parse_error(path, lineno,
                        "arrival time %d is earlier than %d on line %d; jobs must be "
                        "listed in non-decreasing order of arrival time",
                        p->arrivaltime, prev_arrival, prev_line);
            pcb_free(p);
            errors++;
            continue;
        }
        prev_arrival = p->arrivaltime;
        prev_line = lineno;
        p->jobid = q->count + 1;
        enqueue(q, p);
    }

    if (result == EXIT_OK && ferror(fp)) {
        fprintf(stderr, "error: failed while reading '%s': %s\n", path, strerror(errno));
        result = EXIT_INPUT;
    }
    free(buf);
    fclose(fp);

    if (result == EXIT_OK && errors > 0) {
        fprintf(stderr, "error: %d invalid line(s) in '%s'; no jobs were started.\n", errors, path);
        result = EXIT_INPUT;
    }
    if (result == EXIT_OK && q->count == 0) {
        fprintf(stderr, "error: '%s' contains no jobs (only blank lines or comments).\n", path);
        result = EXIT_INPUT;
    }
    if (result != EXIT_OK)
        queue_free(q);
    return result;
}

/* ------------------------------------------------------------------------ */
/* Job records (what the summary table prints)                               */
/* ------------------------------------------------------------------------ */

/* Move a finished PCB's information into its record, then free the PCB. */
static void retire_job(Dispatcher *d, PcbPtr p, TermReason reason, int exec_errno)
{
    JobRecord *r = &d->records[p->jobid - 1];

    r->jobid = p->jobid;
    r->command = p->cmdline;   /* take ownership of the string */
    p->cmdline = NULL;
    r->arrival = p->arrivaltime;
    r->budget = p->cputime;
    r->pid = p->pid;
    r->start = p->starttick;
    r->finish = p->finishtick;
    r->started = (p->state != STATE_READY);
    r->elapsed = r->started ? ts_diff_sec(p->start_mono, p->end_mono) : 0.0;
    r->waitstatus = p->waitstatus;
    r->exec_errno = exec_errno;
    r->reason = reason;
    r->run_ticks = p->run_ticks;
    r->suspensions = p->suspensions;

    p->state = STATE_TERMINATED;
    pcb_free(p);
}

/* ------------------------------------------------------------------------ */
/* Child lifecycle: launch, reap, terminate                                  */
/* ------------------------------------------------------------------------ */

/*
 * Called after waitpid() has returned the status of the current child.
 * Works out why it ended, prints the event, and frees the PCB.
 */
static void finish_current(Dispatcher *d, int status)
{
    PcbPtr p = d->current;
    TermReason reason;

    p->waitstatus = status;
    p->end_mono = mono_now();
    p->finishtick = d->clock;
    double secs = ts_diff_sec(p->start_mono, p->end_mono);

    if (p->budget_expired)
        reason = p->used_sigkill ? REASON_BUDGET_SIGKILL : REASON_BUDGET_SIGINT;
    else if (p->interrupted)
        reason = REASON_INTERRUPTED;
    else if (WIFEXITED(status))
        reason = WEXITSTATUS(status) == 0 ? REASON_COMPLETED : REASON_EXIT_FAILURE;
    else
        reason = REASON_SIGNALED;

    if (WIFEXITED(status))
        log_event(d, "Child reaped: PID %d exited with status %d after %.2f s%s.",
                  (int)p->pid, WEXITSTATUS(status), secs,
                  reason == REASON_COMPLETED ? " (completed normally)" : "");
    else if (WIFSIGNALED(status))
        log_event(d, "Child reaped: PID %d was terminated by signal %d (%s) after %.2f s.",
                  (int)p->pid, WTERMSIG(status), signal_name(WTERMSIG(status)), secs);

    d->current = NULL;
    retire_job(d, p, reason, 0);
}

/* Non-blocking check of the current child. Returns true if it was reaped. */
static bool poll_current(Dispatcher *d)
{
    int status;
    pid_t r;

    if (!d->current)
        return false;
    do {
        r = waitpid(d->current->pid, &status, WNOHANG);
    } while (r == -1 && errno == EINTR);

    if (r == 0)
        return false; /* still running */
    if (r == -1) {
        /* Should not happen for our own child; treat it as lost. */
        fprintf(stderr, "warning: waitpid(%d) failed: %s\n", (int)d->current->pid, strerror(errno));
        status = 0;
    }
    finish_current(d, status);
    return true;
}

/*
 * Stop the current child: send `sig`, wait up to KILL_GRACE_MS for it to
 * exit, and if it is still alive send SIGKILL (which cannot be ignored),
 * then reap it. Only the PID of our own tracked child is ever signalled.
 */
static void terminate_current(Dispatcher *d, int sig)
{
    PcbPtr p = d->current;
    int status;
    pid_t r;

    if (!p || p->pid <= 0) /* never kill(0) or kill(-1): those hit many processes */
        return;

    log_event(d, "Sending %s to PID %d.", signal_name(sig), (int)p->pid);
    if (kill(p->pid, sig) == -1 && errno != ESRCH)
        fprintf(stderr, "warning: kill(%d, %s) failed: %s\n", (int)p->pid, signal_name(sig), strerror(errno));
    if (p->state == STATE_SUSPENDED) {
        /* A stopped process cannot act on SIGINT until it runs again. */
        kill(p->pid, SIGCONT);
        p->state = STATE_RUNNING;
    }

    for (int waited = 0; waited < KILL_GRACE_MS; waited += (int)(POLL_NS / 1000000L)) {
        if (poll_current(d))
            return; /* exited in time; already reaped and freed */
        sleep_ns_full(POLL_NS);
    }
    if (poll_current(d))
        return;

    log_event(d, "PID %d did not exit within %d ms of %s; sending SIGKILL (last resort).",
              (int)p->pid, KILL_GRACE_MS, signal_name(sig));
    p->used_sigkill = true;
    if (kill(p->pid, SIGKILL) == -1 && errno != ESRCH)
        fprintf(stderr, "warning: kill(%d, SIGKILL) failed: %s\n", (int)p->pid, strerror(errno));
    do {
        r = waitpid(p->pid, &status, 0); /* SIGKILL always works, so this returns */
    } while (r == -1 && errno == EINTR);
    if (r == -1)
        status = 0;
    finish_current(d, status);
}

/*
 * Start a job with fork() + execvp().
 *
 * How the parent learns that exec failed: before forking we create a pipe
 * whose ends close automatically on a successful exec (FD_CLOEXEC). If exec
 * fails, the child writes errno into the pipe. The parent reads the pipe:
 * 0 bytes means exec succeeded; sizeof(int) bytes means it failed.
 *
 * Returns true if the job is now running as d->current.
 */
static bool launch_job(Dispatcher *d, PcbPtr p)
{
    int fds[2];
    pid_t parent = getpid();

    p->starttick = d->clock;
    p->start_mono = mono_now();
    log_event(d, "Starting Job %d: %s", p->jobid, p->cmdline);

    if (pipe(fds) == -1) {
        log_event(d, "ERROR: pipe() failed for Job %d: %s", p->jobid, strerror(errno));
        p->state = STATE_TERMINATED;
        p->finishtick = d->clock;
        p->end_mono = p->start_mono;
        retire_job(d, p, REASON_FORK_FAILED, errno);
        return false;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    fflush(stdout); /* so buffered text is not duplicated into the child */
    fflush(stderr);

    pid_t pid = fork();
    if (pid == -1) {
        int err = errno;
        close(fds[0]);
        close(fds[1]);
        log_event(d, "ERROR: fork() failed for Job %d: %s", p->jobid, strerror(err));
        p->state = STATE_TERMINATED;
        p->finishtick = d->clock;
        p->end_mono = p->start_mono;
        retire_job(d, p, REASON_FORK_FAILED, err);
        return false;
    }

    if (pid == 0) {
        /* ---------------- child process ---------------- */
        close(fds[0]);
        setpgid(0, 0); /* own process group: terminal Ctrl+C goes to the dispatcher only */
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL); /* die if the dispatcher is killed -9 */
        if (getppid() != parent)
            _exit(127);
#else
        (void)parent;
#endif
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        int devnull = open("/dev/null", O_RDONLY); /* jobs must not read the terminal */
        if (devnull != -1) {
            dup2(devnull, STDIN_FILENO);
            if (devnull != STDIN_FILENO)
                close(devnull);
        }
        execvp(p->args[0], p->args);
        int err = errno; /* only reached if exec failed */
        ssize_t ignored = write(fds[1], &err, sizeof err);
        (void)ignored;
        _exit(127);
    }

    /* ---------------- parent (dispatcher) ---------------- */
    setpgid(pid, pid); /* same as the child's call; avoids a race. Errors are harmless. */
    close(fds[1]);
    int child_errno = 0;
    ssize_t got;
    do {
        got = read(fds[0], &child_errno, sizeof child_errno);
    } while (got == -1 && errno == EINTR);
    close(fds[0]);

    p->pid = pid;
    p->state = STATE_RUNNING;

    if (got == (ssize_t)sizeof child_errno) {
        int status;
        pid_t r;
        do {
            r = waitpid(pid, &status, 0); /* reap the failed child: no zombie */
        } while (r == -1 && errno == EINTR);
        p->waitstatus = (r == pid) ? status : 0;
        p->end_mono = mono_now();
        p->finishtick = d->clock;
        log_event(d, "ERROR: could not execute '%s': %s. Child PID %d reaped; skipping Job %d.",
                  p->args[0], strerror(child_errno), (int)pid, p->jobid);
        retire_job(d, p, REASON_EXEC_FAILED, child_errno);
        return false;
    }

    if (p->remainingcputime == NO_BUDGET)
        log_event(d, "Child launched with PID %d (no CPU-time budget: runs until it exits).", (int)pid);
    else
        log_event(d, "Child launched with PID %d. CPU-time budget: %d tick(s).",
                  (int)pid, p->remainingcputime);
    d->current = p;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Round-Robin: suspend and resume                                           */
/* ------------------------------------------------------------------------ */

/*
 * RR preemption: pause the current job and put it at the back of the RR
 * queue. SIGTSTP is the "terminal stop" signal (what Ctrl+Z sends); its
 * default action stops the process. A program may catch or ignore SIGTSTP,
 * so if the child has not stopped within STOP_GRACE_MS we send SIGSTOP, which
 * cannot be caught or ignored. waitpid(..., WUNTRACED) confirms the stop.
 */
static void suspend_current(Dispatcher *d)
{
    PcbPtr p = d->current;
    int status;
    pid_t r;

    log_event(d, "Quantum used up and Job %d is waiting: sending SIGTSTP to PID %d.",
              d->rrqueue.head->jobid, (int)p->pid);
    if (kill(p->pid, SIGTSTP) == -1 && errno != ESRCH)
        fprintf(stderr, "warning: kill(%d, SIGTSTP) failed: %s\n", (int)p->pid, strerror(errno));

    for (int waited = 0;; waited += (int)(STOP_POLL_NS / 1000000L)) {
        do {
            r = waitpid(p->pid, &status, WNOHANG | WUNTRACED);
        } while (r == -1 && errno == EINTR);
        if (r == p->pid && WIFSTOPPED(status))
            break; /* it has stopped */
        if (r == p->pid || r == -1) {
            finish_current(d, r == -1 ? 0 : status); /* it exited instead */
            return;
        }
        if (waited >= STOP_GRACE_MS) {
            log_event(d, "PID %d did not stop within %d ms of SIGTSTP; sending SIGSTOP (cannot be ignored).",
                      (int)p->pid, STOP_GRACE_MS);
            p->used_sigstop = true;
            kill(p->pid, SIGSTOP);
            do {
                r = waitpid(p->pid, &status, WUNTRACED);
            } while (r == -1 && errno == EINTR);
            if (r != p->pid || !WIFSTOPPED(status)) {
                finish_current(d, r == p->pid ? status : 0);
                return;
            }
            break;
        }
        sleep_ns_full(STOP_POLL_NS);
    }

    p->state = STATE_SUSPENDED;
    p->suspensions++;
    d->current = NULL;
    enqueue(&d->rrqueue, p);
    log_event(d, "PID %d suspended; Job %d goes to the back of the RR queue (position %d).",
              (int)p->pid, p->jobid, d->rrqueue.count);
}

/* RR: continue a suspended job with SIGCONT and make it the running job. */
static void resume_job(Dispatcher *d, PcbPtr p)
{
    if (p->remainingcputime == NO_BUDGET)
        log_event(d, "Resuming Job %d: sending SIGCONT to PID %d.", p->jobid, (int)p->pid);
    else
        log_event(d, "Resuming Job %d: sending SIGCONT to PID %d. Remaining budget: %d",
                  p->jobid, (int)p->pid, p->remainingcputime);
    if (kill(p->pid, SIGCONT) == -1 && errno != ESRCH)
        fprintf(stderr, "warning: kill(%d, SIGCONT) failed: %s\n", (int)p->pid, strerror(errno));
    p->state = STATE_RUNNING;
    p->quantum_used = 0;
    d->current = p;
}

/* RR: move every job whose arrival time has come from the input queue to
 * the back of the RR queue (the sheet's "unload any pending processes"). */
static void unload_arrivals(Dispatcher *d)
{
    while (d->queue.head && d->queue.head->arrivaltime <= d->clock) {
        PcbPtr p = dequeue(&d->queue);
        enqueue(&d->rrqueue, p);
        log_event(d, "Job %d arrived (%s); moved from the input queue to the RR queue (position %d).",
                  p->jobid, p->cmdline, d->rrqueue.count);
    }
}

/* ------------------------------------------------------------------------ */
/* The dispatcher loop (FCFS and Round-Robin)                                */
/* ------------------------------------------------------------------------ */

static bool work_left(const Dispatcher *d)
{
    return d->queue.head || d->rrqueue.head || d->current;
}

/* Remember which job holds the CPU during the current tick (Gantt chart). */
static void record_tick(Dispatcher *d)
{
    if (d->clock >= d->timeline_cap) {
        long cap = d->timeline_cap ? d->timeline_cap * 2 : 64;
        while (cap <= d->clock)
            cap *= 2;
        int *t = realloc(d->timeline, (size_t)cap * sizeof *t);
        if (!t)
            return; /* the chart is optional: just stop recording */
        for (long i = d->timeline_cap; i < cap; i++)
            t[i] = 0;
        d->timeline = t;
        d->timeline_cap = cap;
    }
    d->timeline[d->clock] = d->current ? d->current->jobid : 0;
    if (d->clock + 1 > d->timeline_len)
        d->timeline_len = d->clock + 1;
}

/*
 * Steps (d)-(e): if no job is running, dequeue the head of the queue when its
 * arrival time has been reached, and launch it. If a launch fails, the next
 * eligible job is tried at once (a failed launch uses no CPU time).
 * Only the HEAD is ever considered, so a later job can never overtake an
 * earlier one. Returns true if a job is running afterwards.
 */
static bool dispatch_next(Dispatcher *d)
{
    while (!d->current && d->queue.head && d->queue.head->arrivaltime <= d->clock) {
        PcbPtr p = dequeue(&d->queue);
        log_event(d, "Job %d is eligible (arrived at T=%d) and is first in the queue.",
                  p->jobid, p->arrivaltime);
        launch_job(d, p);
    }
    return d->current != NULL;
}

/*
 * RR version: if no job is running, take the head of the RR queue. A job
 * that was suspended is restarted with SIGCONT; a new job is started with
 * fork() + exec().
 */
static bool dispatch_next_rr(Dispatcher *d)
{
    while (!d->current && d->rrqueue.head) {
        PcbPtr p = dequeue(&d->rrqueue);
        if (p->state == STATE_SUSPENDED) {
            resume_job(d, p);
        } else {
            log_event(d, "Job %d is at the head of the RR queue: first run.", p->jobid);
            launch_job(d, p);
        }
    }
    return d->current != NULL;
}

static bool dispatch(Dispatcher *d)
{
    return d->rr ? dispatch_next_rr(d) : dispatch_next(d);
}

/*
 * Wait until the next tick boundary (absolute monotonic deadline, so small
 * delays do not accumulate into drift). While waiting, check every 50 ms
 * whether the current child has exited, so it is reaped promptly (no zombie
 * lingers). If it has, the CPU is free, so the next eligible job is
 * dispatched immediately instead of leaving the CPU idle until the next tick.
 * Returns early if a stop signal (Ctrl+C) arrives or no work is left.
 */
static void wait_for_tick(Dispatcher *d)
{
    struct timespec now = mono_now();

    d->next_tick = ts_add_ns(d->next_tick, TICK_NS);
    if (ts_cmp(now, d->next_tick) > 0)
        d->next_tick = now; /* we fell behind (rare); resynchronise */

    while (!g_stop_signal) {
        now = mono_now();
        if (ts_cmp(now, d->next_tick) >= 0)
            break;
        long left = (long)((d->next_tick.tv_sec - now.tv_sec) * NS_PER_SEC +
                           (d->next_tick.tv_nsec - now.tv_nsec));
        struct timespec slice = { 0, left < POLL_NS ? left : POLL_NS };
        nanosleep(&slice, NULL); /* EINTR is fine: the loop re-checks the flag */
        if (g_stop_signal)
            break;
        /* If we woke up after the deadline, the tick is over: a child that
         * exited just now belongs to the NEXT tick, so leave it for the main
         * loop to reap after the clock has been incremented. */
        if (ts_cmp(mono_now(), d->next_tick) >= 0)
            break;
        if (d->current && poll_current(d)) {
            /* Child finished between ticks: hand the CPU to the next job now. */
            dispatch(d);
            record_tick(d); /* this tick now belongs to the new job (or idle) */
            if (!work_left(d))
                return; /* everything is done; no need to wait out the tick */
        }
    }
}

/* Announce jobs whose arrival time is exactly now (they become ready). */
static void announce_arrivals(Dispatcher *d)
{
    int ahead = d->current ? 1 : 0; /* the running job counts as ahead */
    for (PcbPtr p = d->queue.head; p; p = p->next) {
        if (p->arrivaltime > d->clock)
            break; /* queue is sorted by arrival: nothing later has arrived */
        if (p->arrivaltime == d->clock)
            log_event(d, "Job %d arrived (%s). Jobs ahead of it: %d.", p->jobid, p->cmdline, ahead);
        ahead++;
    }
}

/* Ctrl+C (or SIGTERM/SIGHUP): stop the running child (and, in RR mode, every
 * suspended child) and discard the jobs that never started. */
static void shut_down(Dispatcher *d)
{
    int sig = (int)g_stop_signal;

    printf("\n");
    log_event(d, "Received %s. Shutting down safely.", signal_name(sig));
    if (d->current) {
        d->current->interrupted = true;
        terminate_current(d, SIGINT);
    }
    PcbPtr p;
    while ((p = dequeue(&d->rrqueue)) != NULL) {
        if (p->state == STATE_SUSPENDED) {
            /* A real, stopped process: terminate and reap it too. */
            log_event(d, "Job %d (PID %d) is suspended; stopping it as well.", p->jobid, (int)p->pid);
            d->current = p;
            p->interrupted = true;
            terminate_current(d, SIGINT);
        } else {
            log_event(d, "Job %d discarded (never started).", p->jobid);
            retire_job(d, p, REASON_NOT_STARTED, 0);
        }
    }
    while ((p = dequeue(&d->queue)) != NULL) {
        log_event(d, "Job %d discarded (never started).", p->jobid);
        retire_job(d, p, REASON_NOT_STARTED, 0);
    }
}

/*
 * The algorithms from the project sheet. FCFS:
 *   while jobs are queued or one is running:
 *     a-c. if a job is running: decrement its budget; if it has run out,
 *          send SIGINT and reap it (natural exits are reaped as they happen)
 *     d-e. if nothing is running and the head job has arrived, dequeue it,
 *          fork+exec it and make it the current process
 *     f-g. sleep one tick, then increment the dispatcher timer
 *
 * Round-Robin (--rr) adds an input queue and an RR queue:
 *   while anything is in either queue or a job is running:
 *     1. move every job whose arrival time has come: input queue -> RR queue
 *     2. if a job is running: decrement its budget; if it has run out send
 *        SIGINT; else if its quantum is used up and other jobs are waiting,
 *        suspend it with SIGTSTP and put it at the back of the RR queue
 *     3. if nothing is running: dequeue the head of the RR queue; SIGCONT it
 *        if it was suspended, otherwise fork+exec it
 *     4. sleep one tick, then increment the dispatcher timer
 *   Step 1 comes before step 2, so a job arriving at the same tick that the
 *   running job is preempted queues AHEAD of the preempted job - exactly the
 *   order in Stallings, Figure 9.5 (RR, q = 1).
 *
 * Timing adjustment (documented in the README): while sleeping through a tick
 * the dispatcher checks every 50 ms whether the child has exited. If it has,
 * the child is reaped at once and the next eligible job is started at once,
 * so the CPU is not left idle for the rest of the tick. This is what makes the
 * schedule match the textbook FCFS Gantt chart (e.g. 0-3, 3-9, 9-13, ...).
 */
static int run_dispatcher(Dispatcher *d)
{
    d->clock = 0;
    d->t0 = mono_now();
    d->next_tick = d->t0;
    log_event(d, "Dispatcher clock initialized to 0.");

    while (work_left(d)) {
        if (g_stop_signal)
            break;

        /* RR step 1: unload arrived jobs from the input queue to the RR queue */
        if (d->rr)
            unload_arrivals(d);

        /* (a)-(c): manage the running process */
        if (d->current && !poll_current(d)) {
            PcbPtr p = d->current;
            bool ended = false;
            p->run_ticks++;
            p->quantum_used++;
            if (p->remainingcputime == NO_BUDGET) {
                log_event(d, "PID %d running (no budget limit).", (int)p->pid);
            } else {
                p->remainingcputime--;
                if (p->remainingcputime > 0) {
                    log_event(d, "PID %d running. Remaining budget: %d", (int)p->pid, p->remainingcputime);
                } else {
                    log_event(d, "CPU-time budget expired for PID %d.", (int)p->pid);
                    p->budget_expired = true;
                    terminate_current(d, SIGINT); /* reaps and frees it */
                    ended = true;
                }
            }
            /* RR: time slice over? Preempt only if someone else is waiting. */
            if (!ended && d->rr && d->current && p->quantum_used >= d->quantum) {
                if (d->rrqueue.head)
                    suspend_current(d);
                else
                    p->quantum_used = 0; /* nobody waiting: it keeps the CPU */
            }
        }
        if (g_stop_signal)
            break;

        if (!d->rr)
            announce_arrivals(d);

        /* (d)-(e): dispatch the next job if the CPU is free */
        dispatch(d);

        if (!d->current) {
            if (!work_left(d))
                break; /* nothing running and nothing left: finished */
            log_event(d, "CPU idle. Next job (Job %d) arrives at T=%d.",
                      d->queue.head->jobid, d->queue.head->arrivaltime);
            d->idle_ticks++;
        }
        record_tick(d);

        /* (f)-(g): wait one tick, then advance the dispatcher clock */
        wait_for_tick(d);
        if (g_stop_signal)
            break;
        if (!work_left(d))
            break; /* the last job finished during this tick */
        d->clock++;
    }

    if (g_stop_signal) {
        shut_down(d);
        return 128 + (int)g_stop_signal;
    }
    log_event(d, "All jobs finished. Dispatcher exiting.");
    return EXIT_OK;
}

/* ------------------------------------------------------------------------ */
/* Output: banner, job table, summary                                        */
/* ------------------------------------------------------------------------ */

static void print_banner(const Dispatcher *d)
{
    printf("=============================================\n");
    printf("        BATCH PROCESS MONITOR  v%s\n", PROGRAM_VERSION);
    if (d->rr)
        printf("        ROUND-ROBIN DISPATCHER (quantum = %d)\n", d->quantum);
    else
        printf("        FCFS PROCESS DISPATCHER\n");
    printf("=============================================\n\n");
}

/*
 * Draw which job held the CPU in each tick, one column per tick, in the same
 * style as Stallings Figure 9.5. Jobs are lettered A, B, C, ... in file order.
 */
static void print_gantt(const Dispatcher *d)
{
    long len = d->timeline_len;
    while (len > 0 && d->timeline[len - 1] == 0)
        len--; /* drop trailing idle ticks */
    if (len == 0)
        return;
    if (len > GANTT_MAX_TICKS) {
        printf("\n(Gantt chart not shown: %ld ticks is wider than %d columns.)\n", len, GANTT_MAX_TICKS);
        return;
    }

    char ruler[GANTT_MAX_TICKS + 8];
    memset(ruler, ' ', sizeof ruler);
    for (long t = 0; t <= len; t += 5) {
        char num[24];
        int n = snprintf(num, sizeof num, "%ld", t);
        if (t + n < (long)sizeof ruler)
            memcpy(ruler + t, num, (size_t)n);
    }
    ruler[len + 3] = '\0';

    printf("\nGantt chart (one column = one tick, # = job holds the CPU):\n");
    printf("            %s\n", ruler);
    printf("            ");
    for (long t = 0; t <= len; t++)
        putchar(t % 5 == 0 ? '|' : '.');
    putchar('\n');
    for (int j = 1; j <= d->njobs; j++) {
        char label[32];
        if (d->njobs <= 26)
            snprintf(label, sizeof label, "%c (Job %d)", 'A' + j - 1, j);
        else
            snprintf(label, sizeof label, "Job %d", j);
        printf("%-12s", label);
        for (long t = 0; t < len; t++)
            putchar(d->timeline[t] == j ? '#' : ' ');
        putchar('\n');
    }
}

/*
 * Copy a command into buf (size sz) for a table column. The directory part of
 * the program path is dropped ("./build/test_worker 1" -> "test_worker 1")
 * and the text is shortened with "..." if it still does not fit.
 */
static const char *fit(const char *s, char *buf, size_t sz)
{
    const char *word_end = s + strcspn(s, " \t");
    for (const char *c = s; c < word_end; c++)
        if (*c == '/')
            s = c + 1;
    size_t len = strlen(s);
    if (len < sz) {
        memcpy(buf, s, len + 1);
    } else {
        memcpy(buf, s, sz - 4);
        memcpy(buf + sz - 4, "...", 4);
    }
    return buf;
}

static void print_job_table(const Queue *q, const char *path)
{
    char cmd[29];
    printf("Loaded %d job%s from %s.\n\n", q->count, q->count == 1 ? "" : "s", path);
    printf("JOB  ARRIVAL  BUDGET  PRIO  MEM   RESOURCES  COMMAND\n");
    printf("---  -------  ------  ----  ----  ---------  ----------------------------\n");
    for (PcbPtr p = q->head; p; p = p->next) {
        if (p->has_metadata)
            printf("%3d  %7d  %6d  %4d  %4d  %d,%d,%d,%d    %s\n", p->jobid, p->arrivaltime,
                   p->cputime, p->priority, p->memory, p->resources[0], p->resources[1],
                   p->resources[2], p->resources[3], fit(p->cmdline, cmd, sizeof cmd));
        else
            printf("%3d  %7d  %6s  %4s  %4s  %-9s  %s\n", p->jobid, p->arrivaltime,
                   "none", "-", "-", "-", fit(p->cmdline, cmd, sizeof cmd));
    }
    printf("\n(Priority, memory and resources are kept as metadata; the dispatcher ignores them.)\n\n");
    fflush(stdout);
}

static void describe_result(const JobRecord *r, char *buf, size_t sz)
{
    switch (r->reason) {
    case REASON_COMPLETED:
        snprintf(buf, sz, "completed (exit 0)");
        break;
    case REASON_EXIT_FAILURE:
        snprintf(buf, sz, "failed (exit %d)", WEXITSTATUS(r->waitstatus));
        break;
    case REASON_BUDGET_SIGINT:
        snprintf(buf, sz, "budget expired: SIGINT");
        break;
    case REASON_BUDGET_SIGKILL:
        snprintf(buf, sz, "budget expired: SIGKILL");
        break;
    case REASON_SIGNALED:
        snprintf(buf, sz, "killed by %s", signal_name(WTERMSIG(r->waitstatus)));
        break;
    case REASON_EXEC_FAILED:
        snprintf(buf, sz, "launch failed: %s", strerror(r->exec_errno));
        break;
    case REASON_FORK_FAILED:
        snprintf(buf, sz, "fork failed: %s", strerror(r->exec_errno));
        break;
    case REASON_INTERRUPTED:
        snprintf(buf, sz, "interrupted (Ctrl+C)");
        break;
    case REASON_NOT_STARTED:
        snprintf(buf, sz, "not started");
        break;
    default:
        snprintf(buf, sz, "unknown");
        break;
    }
}

static void print_summary(const Dispatcher *d)
{
    char cmd[29], budget[24], pid[24], start[24], finish[24], wait[24], turn[24], secs[32], result[64];
    long total_wait = 0, total_turn = 0;
    int started = 0;

    printf("\nFinal execution summary (START/FINISH/WAIT/TURN in dispatcher ticks, ELAPSED in real seconds):\n");
    printf("JOB  PID      COMMAND                       ARRIVE  BUDGET  START  FINISH  WAIT  TURN  ELAPSED  RESULT\n");
    printf("---  -------  ----------------------------  ------  ------  -----  ------  ----  ----  -------  ------------------------\n");
    for (int i = 0; i < d->njobs; i++) {
        const JobRecord *r = &d->records[i];
        if (r->jobid == 0)
            continue;
        snprintf(budget, sizeof budget, r->budget == NO_BUDGET ? "none" : "%d", r->budget);
        if (r->started) {
            snprintf(pid, sizeof pid, r->pid > 0 ? "%d" : "-", (int)r->pid);
            snprintf(start, sizeof start, "%ld", r->start);
            snprintf(finish, sizeof finish, "%ld", r->finish);
            /* FCFS: waiting time = start - arrival. RR: a job also waits in
             * the RR queue between slices, so waiting = turnaround - CPU ticks. */
            long w = d->rr ? (r->finish - r->arrival) - r->run_ticks : r->start - r->arrival;
            if (w < 0)
                w = 0;
            snprintf(wait, sizeof wait, "%ld", w);
            snprintf(turn, sizeof turn, "%ld", r->finish - r->arrival);
            snprintf(secs, sizeof secs, "%.2f", r->elapsed);
            total_wait += w;
            total_turn += r->finish - r->arrival;
            started++;
        } else {
            snprintf(pid, sizeof pid, "-");
            snprintf(start, sizeof start, "-");
            snprintf(finish, sizeof finish, "-");
            snprintf(wait, sizeof wait, "-");
            snprintf(turn, sizeof turn, "-");
            snprintf(secs, sizeof secs, "-");
        }
        describe_result(r, result, sizeof result);
        printf("%3d  %-7s  %-28s  %6d  %6s  %5s  %6s  %4s  %4s  %7s  %s\n", r->jobid, pid,
               fit(r->command ? r->command : "", cmd, sizeof cmd), r->arrival, budget, start,
               finish, wait, turn, secs, result);
    }
    if (started > 0)
        printf("\nJobs started: %d of %d.  Average waiting time: %.2f ticks.  Average turnaround: %.2f ticks.\n",
               started, d->njobs, (double)total_wait / started, (double)total_turn / started);
    printf("Dispatcher ran for %ld tick(s) (%.2f s real time), CPU idle for %ld tick(s).\n",
           d->clock, ts_diff_sec(d->t0, mono_now()), d->idle_ticks);
    if (d->rr) {
        int susp = 0;
        for (int i = 0; i < d->njobs; i++)
            susp += d->records[i].suspensions;
        printf("Round-Robin: %d suspension(s) with SIGTSTP; WAIT = turnaround - ticks on the CPU.\n", susp);
    }
    print_gantt(d);
    fflush(stdout);
}

/* ------------------------------------------------------------------------ */
/* Command line and main()                                                   */
/* ------------------------------------------------------------------------ */

static void on_stop_signal(int sig)
{
    g_stop_signal = sig; /* only set a flag: everything else happens in the main loop */
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    const int sigs[] = { SIGINT, SIGTERM, SIGHUP };

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: let sleeps return early with EINTR */
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        if (sigaction(sigs[i], &sa, NULL) == -1)
            return -1;
    return 0;
}

/*
 * Put the directory containing this dispatcher first in PATH, so that job
 * commands can name the programs built next to it (e.g. "0,sleepy 3" from the
 * project sheet finds build/sleepy). Children inherit PATH through fork/exec.
 */
static void add_own_dir_to_path(const char *argv0)
{
    char *exe = NULL;
#ifdef __linux__
    exe = realpath("/proc/self/exe", NULL); /* works however we were started */
#endif
    if (!exe && strchr(argv0, '/'))
        exe = realpath(argv0, NULL);
    if (!exe)
        return; /* started through PATH: its directory is already searchable */

    char *slash = strrchr(exe, '/');
    if (slash == exe)
        slash[1] = '\0'; /* the dispatcher is in "/" */
    else if (slash)
        *slash = '\0';

    const char *old = getenv("PATH");
    if (!old || !*old)
        old = "/usr/local/bin:/usr/bin:/bin";
    size_t n = strlen(exe) + 1 + strlen(old) + 1;
    char *path = malloc(n);
    if (path) {
        snprintf(path, n, "%s:%s", exe, old);
        setenv("PATH", path, 1);
        free(path);
    }
    free(exe);
}

static void print_usage(FILE *out)
{
    fprintf(out,
            "Usage: %s [--fcfs] <dispatch-file>                 First-Come, First-Served (default)\n"
            "       %s --rr [--quantum N] <dispatch-file>       Round-Robin, time quantum N ticks (default 1)\n"
            "       %s --help | --version\n"
            "\n"
            "Runs the jobs in <dispatch-file> using a 1-second dispatcher tick. FCFS runs\n"
            "one job at a time to completion. Round-Robin gives each job a time slice;\n"
            "when it is used up and another job is waiting, the job is suspended with\n"
            "SIGTSTP (SIGSTOP if ignored), queued again, and later resumed with SIGCONT.\n"
            "\n"
            "Dispatch-file lines (blank lines and lines starting with # are ignored):\n"
            "  <arrival>,<command and arguments>\n"
            "  <arrival>,<priority>,<cpu>,<memory>,<r1>,<r2>,<r3>,<r4>[,<command and arguments>]\n"
            "\n"
            "<cpu> is a budget in ticks; when it runs out the job is sent SIGINT\n"
            "(then SIGKILL if it has not exited after %d ms). The simple format has\n"
            "no budget: the job runs until it exits. A full-format line without a\n"
            "command runs the default program 'sleepy' until its budget expires.\n"
            "Lines must be in non-decreasing arrival order. Words in a command may be\n"
            "grouped with quotes. Programs built next to the dispatcher (sleepy,\n"
            "test_worker) can be named without a path.\n"
            "\n"
            "Examples:  %s tests/jobs_sheet_processlist.txt\n"
            "           %s --rr tests/jobs_rr_stallings.txt\n",
            PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, KILL_GRACE_MS, PROGRAM_NAME, PROGRAM_NAME);
}

int main(int argc, char *argv[])
{
    Dispatcher d;
    int rc;

    setvbuf(stdout, NULL, _IOLBF, 0); /* line-buffered, even when piped to a file */

    memset(&d, 0, sizeof d);
    queue_init(&d.queue);
    queue_init(&d.rrqueue);
    d.quantum = 1;

    const char *file = NULL;
    bool quantum_given = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(stdout);
            return EXIT_OK;
        } else if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
            printf("%s %s (FCFS and Round-Robin dispatchers)\n", PROGRAM_NAME, PROGRAM_VERSION);
            return EXIT_OK;
        } else if (strcmp(a, "--rr") == 0) {
            d.rr = true;
        } else if (strcmp(a, "--fcfs") == 0) {
            d.rr = false;
        } else if (strcmp(a, "--quantum") == 0 || strcmp(a, "-q") == 0) {
            int q;
            if (i + 1 >= argc || !parse_int(argv[i + 1], &q) || q < 1 || q > 3600) {
                fprintf(stderr, "error: %s needs a whole number of ticks from 1 to 3600.\n\n", a);
                print_usage(stderr);
                return EXIT_USAGE;
            }
            d.quantum = q;
            quantum_given = true;
            i++;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "error: unknown option '%s'.\n\n", a);
            print_usage(stderr);
            return EXIT_USAGE;
        } else if (file) {
            fprintf(stderr, "error: expected exactly one dispatch file.\n\n");
            print_usage(stderr);
            return EXIT_USAGE;
        } else {
            file = a;
        }
    }
    if (!file) {
        fprintf(stderr, "error: expected exactly one dispatch file.\n\n");
        print_usage(stderr);
        return EXIT_USAGE;
    }
    if (quantum_given && !d.rr) {
        fprintf(stderr, "error: --quantum only applies to Round-Robin; add --rr.\n\n");
        print_usage(stderr);
        return EXIT_USAGE;
    }

    add_own_dir_to_path(argv[0]);

    print_banner(&d);
    rc = load_dispatch_file(file, &d.queue);
    if (rc != EXIT_OK)
        return rc;

    d.njobs = d.queue.count;
    d.records = calloc((size_t)d.njobs, sizeof *d.records);
    if (!d.records) {
        fprintf(stderr, "error: out of memory\n");
        queue_free(&d.queue);
        return EXIT_SYSTEM;
    }
    if (install_signal_handlers() == -1) {
        fprintf(stderr, "error: cannot install signal handlers: %s\n", strerror(errno));
        queue_free(&d.queue);
        free(d.records);
        return EXIT_SYSTEM;
    }

    print_job_table(&d.queue, file);
    rc = run_dispatcher(&d);
    print_summary(&d);

    for (int i = 0; i < d.njobs; i++)
        free(d.records[i].command);
    free(d.records);
    free(d.timeline);
    queue_free(&d.queue);   /* already empty; kept for safety */
    queue_free(&d.rrqueue);
    return rc;
}
