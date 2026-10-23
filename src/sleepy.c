/*
 * sleepy.c - the simple job program used in the project sheet's examples.
 *
 * Usage:  sleepy [seconds]
 *
 *   sleepy 3    sleeps for 3 seconds, printing one line per second, then exits 0.
 *   sleepy      (no argument) keeps running, one line per second, until it is
 *               stopped by a signal. The dispatcher runs this for process-list
 *               lines that have no command ("0, 0, 3, 64, 0, 0, 0, 0"), so the
 *               job lasts exactly as long as its CPU-time budget: when the
 *               budget expires, the dispatcher's SIGINT ends it.
 *
 * Build:  gcc -std=c11 -Wall -Wextra -Wpedantic -o sleepy sleepy.c
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    long seconds = -1; /* -1 = run until a signal stops us */

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 2) {
        fprintf(stderr, "Usage: sleepy [seconds]\n");
        return 2;
    }
    if (argc == 2) {
        char *end;
        errno = 0;
        seconds = strtol(argv[1], &end, 10);
        if (*argv[1] == '\0' || *end != '\0' || errno == ERANGE || seconds < 0 || seconds > 86400) {
            fprintf(stderr, "sleepy: '%s' is not a number of seconds (0-86400)\n", argv[1]);
            return 2;
        }
    }

    pid_t pid = getpid();
    if (seconds < 0)
        printf("    [sleepy pid=%d] started: running until stopped\n", (int)pid);
    else
        printf("    [sleepy pid=%d] started: sleeping %ld s\n", (int)pid, seconds);

    for (long t = 1; seconds < 0 || t <= seconds; t++) {
        struct timespec req = { 1, 0 }, rem;
        while (nanosleep(&req, &rem) == -1 && errno == EINTR)
            req = rem;
        printf("    [sleepy pid=%d] tick %ld\n", (int)pid, t);
    }
    printf("    [sleepy pid=%d] done\n", (int)pid);
    return 0;
}
