/*
 * test_worker.c - a safe, predictable job for testing the Batch Process Monitor.
 *
 * Usage: test_worker <seconds> [--exit CODE] [--ignore-sigint] [--abort]
 *                              [--busy] [--name LABEL]
 *
 *   <seconds>        how long to run (0-3600), printing progress once a second
 *   --exit CODE      exit with this status (0-255) when finished (default 0)
 *   --ignore-sigint  ignore SIGINT, to test the dispatcher's SIGKILL fallback
 *   --abort          end abnormally with SIGABRT instead of exiting
 *                    (core dumps are disabled first, so no core file is written)
 *   --busy           burn CPU in a loop instead of sleeping, to show the
 *                    difference between real CPU use and the dispatcher's
 *                    elapsed-time budget
 *   --name LABEL     label printed on every line (default "worker")
 *
 * It only sleeps (or spins) and prints; it changes no files, network or system
 * settings.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static int parse_range(const char *s, int lo, int hi, int *out)
{
    char *end;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || errno == ERANGE || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "Usage: test_worker <seconds> [--exit CODE] [--ignore-sigint] "
                    "[--abort] [--busy] [--name LABEL]\n");
}

static double now_sec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Wait one second: sleep (no CPU used), or spin (100% CPU) in busy mode. */
static void one_second(int busy)
{
    if (busy) {
        volatile unsigned long spin = 0;
        double until = now_sec() + 1.0;
        while (now_sec() < until)
            spin++;
        return;
    }
    struct timespec req = { 1, 0 }, rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
        req = rem;
}

int main(int argc, char *argv[])
{
    int seconds, code = 0, ignore = 0, do_abort = 0, busy = 0;
    const char *name = "worker";

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2 || parse_range(argv[1], 0, 3600, &seconds) != 0) {
        usage();
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--exit") == 0 && i + 1 < argc) {
            if (parse_range(argv[++i], 0, 255, &code) != 0) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--ignore-sigint") == 0) {
            ignore = 1;
        } else if (strcmp(argv[i], "--abort") == 0) {
            do_abort = 1;
        } else if (strcmp(argv[i], "--busy") == 0) {
            busy = 1;
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else {
            usage();
            return 2;
        }
    }

    if (ignore) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
    }

    printf("    [%s pid=%d] started: will run %d s (%s), then %s%s\n", name, (int)getpid(),
           seconds, busy ? "busy CPU loop" : "sleeping",
           do_abort ? "abort with SIGABRT" : "exit", ignore ? " (ignoring SIGINT)" : "");
    for (int t = 1; t <= seconds; t++) {
        one_second(busy);
        printf("    [%s pid=%d] progress %d/%d s\n", name, (int)getpid(), t, seconds);
    }

    if (busy) {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0)
            printf("    [%s pid=%d] actual CPU time used: %.2f s user + %.2f s system\n",
                   name, (int)getpid(),
                   (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6,
                   (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6);
    }

    if (do_abort) {
        struct rlimit no_core = { 0, 0 };
        setrlimit(RLIMIT_CORE, &no_core); /* never leave a core file behind */
        printf("    [%s pid=%d] aborting now (simulated crash)\n", name, (int)getpid());
        abort();
    }
    printf("    [%s pid=%d] finished, exiting with status %d\n", name, (int)getpid(), code);
    return code;
}
