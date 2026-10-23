#!/usr/bin/env bash
# run_tests.sh - automated tests for the Batch Process Monitor.
#
# Usage (from the repository root):   make test
#                               or:   bash tests/run_tests.sh
#   QUICK=1 make test   skips the four 20-second specification examples.
#
# Each test runs the real dispatcher with real child processes and checks the
# exit code and the output. After every test it also checks that no test_worker
# process was left running (no orphans).

set -u
cd "$(dirname "$0")/.." || exit 1

BM=./build/batch_monitor
LOGDIR=build/test_logs
TMPDIR_T=build/test_tmp
mkdir -p "$LOGDIR" "$TMPDIR_T"
export NO_COLOR=1

PASS=0
FAIL=0
FAILED_NAMES=()
CURRENT=""
CURRENT_OK=1

if [[ ! -x $BM || ! -x build/test_worker || ! -x build/sleepy ]]; then
    echo "Build first: make"
    exit 1
fi

begin() { CURRENT="$1"; CURRENT_OK=1; printf '%-62s ' "$1"; }
bad()   { CURRENT_OK=0; DETAILS+=("    - $1"); }
end() {
    if pgrep -x 'test_worker|sleepy' >/dev/null 2>&1; then
        bad "a test_worker process is still running after the test (orphan)"
        pkill -KILL -x 'test_worker|sleepy' 2>/dev/null
    fi
    if (( CURRENT_OK )); then
        echo "PASS"; PASS=$((PASS + 1))
    else
        echo "FAIL"; FAIL=$((FAIL + 1)); FAILED_NAMES+=("$CURRENT")
        printf '%s\n' "${DETAILS[@]}"
        echo "    (full output: $LOG)"
    fi
    DETAILS=()
}
DETAILS=()

# run NAME EXPECTED_EXIT FILE  -> runs the dispatcher, output in $LOG
run() {
    LOG="$LOGDIR/$1.log"
    timeout 90 "$BM" "${@:3}" >"$LOG" 2>&1
    RC=$?
    [[ $RC -eq $2 ]] || bad "exit code $RC, expected $2"
}
expect()    { grep -qF -- "$1" "$LOG" || bad "missing output: $1"; }
expect_no() { ! grep -qF -- "$1" "$LOG" || bad "unexpected output: $1"; }
# The row of the final summary for job N, from the ARRIVE column onwards:
#   ARRIVE BUDGET START FINISH WAIT TURN ELAPSED RESULT...
row() { grep -E "^ {0,2}$1  " "$LOG" | tail -1 | cut -c45-; }
expect_times() { # JOB START FINISH
    local got
    got=$(row "$1" | awk '{print $3, $4}')
    [[ $got == "$2 $3" ]] || bad "job $1 start/finish = '$got', expected '$2 $3'"
}
expect_result() { # JOB TEXT
    row "$1" | grep -qF -- "$2" || bad "job $1 result is not '$2': $(row "$1")"
}
# Lines containing PAT1, PAT2, ... must appear in that order.
expect_order() {
    local last=0 n
    for pat in "$@"; do
        n=$(grep -nF -- "$pat" "$LOG" | head -1 | cut -d: -f1)
        if [[ -z $n || $n -le $last ]]; then bad "wrong order at: $pat"; return; fi
        last=$n
    done
}

echo "Batch Process Monitor - automated tests"
echo "========================================"

# ---------------------------------------------------------------- CLI ----
begin "CLI: --help prints usage, exit 0"
run cli_help 0 --help; expect "Usage: batch_monitor <dispatch-file>"; end
begin "CLI: --version, exit 0"
run cli_version 0 --version; expect "batch_monitor 1.0.0"; end
begin "CLI: no arguments -> usage error, exit 1"
run cli_noargs 1; expect "expected exactly one dispatch file"; end
begin "CLI: unknown option -> usage error, exit 1"
run cli_badopt 1 --bogus; expect "unknown option '--bogus'"; end
begin "CLI: two files -> usage error, exit 1"
run cli_twofiles 1 tests/jobs_single.txt tests/jobs_single.txt; end

# --------------------------------------------------------- scheduling ----
begin "1. one job arriving at time zero"
run t01_single 0 tests/jobs_single.txt
expect "Child launched with PID"; expect "Remaining budget: 4"
expect_times 1 0 2; expect_result 1 "completed (exit 0)"; end

begin "2+15. several jobs at T=0 run in file order (A, B, C)"
run t02_equal 0 tests/jobs_equal_arrival.txt
expect_order "[A pid=" "[B pid=" "[C pid="
expect_times 1 0 1; expect_times 2 1 2; expect_times 3 2 3; end

begin "3+4. different arrival times, idle CPU before arrivals"
run t03_arrivals 0 tests/jobs_arrival_times.txt
expect "CPU idle. Next job (Job 1) arrives at T=2."
expect "Job 2 arrived"; expect "CPU idle. Next job (Job 3) arrives at T=9."
expect_times 1 2 4; expect_times 2 4 5; expect_times 3 9 10; end

begin "5. jobs that exit naturally before their budget expires"
run t05_early 0 tests/jobs_early_exit.txt
expect "[early bird pid="   # quoted argument with a space kept as one word
expect_result 1 "completed (exit 0)"; expect_result 2 "completed (exit 0)"
expect_no "budget expired"; end

begin "6+13. budget expiry: SIGINT, and SIGKILL fallback if ignored"
run t06_budget 0 tests/jobs_budget_expiry.txt
expect "CPU-time budget expired for PID"
expect "did not exit within 500 ms of SIGINT; sending SIGKILL"
expect_times 1 0 3; expect_result 1 "budget expired: SIGINT"
expect_times 2 3 5; expect_result 2 "budget expired: SIGKILL"; end

begin "7+11. missing executables, nonzero exit, crash (SIGABRT)"
LOG="$LOGDIR/t07_errors.log"
timeout 90 "$BM" tests/jobs_error_cases.txt >"$LOG" 2>&1 &
BG=$!
sleep 1.5
# While it runs, the dispatcher must have no zombie children.
DPID=$(pgrep -P "$BG" -x batch_monitor)   # $BG is `timeout`; the dispatcher is its child
[[ -n $DPID ]] || bad "could not find the dispatcher process"
if ps -o stat= --ppid "${DPID:-0}" 2>/dev/null | grep -q Z; then bad "zombie child found while running"; fi
wait "$BG"; RC=$?
[[ $RC -eq 0 ]] || bad "exit code $RC, expected 0"
expect "could not execute './build/no_such_program': No such file or directory"
expect "could not execute 'definitely_not_a_real_command_25': No such file or directory"
expect_result 1 "launch failed"; expect_result 2 "launch failed"
expect_result 3 "failed (exit 3)"; expect_result 4 "completed (exit 0)"
expect_result 5 "killed by SIGABRT"
[[ -z $(ls core* 2>/dev/null) ]] || bad "a core file was written"
end

# ------------------------------------------------------------ parsing ----
begin "8. malformed records rejected with line numbers"
run t08_malformed 2 tests/jobs_malformed.txt
expect "jobs_malformed.txt:4: error: arrival time 'abc' is not a valid integer"
expect "jobs_malformed.txt:5: error: arrival time must not be negative (got -1)"
expect "jobs_malformed.txt:6: error: CPU time must be at least 1 tick (got 0)"
expect "jobs_malformed.txt:7: error: resource 4 allocation must not be negative (got -5)"
expect "jobs_malformed.txt:8: error: metadata format needs 8 numeric fields"
expect "jobs_malformed.txt:9: error: CPU time 'x' is not a valid integer"
expect "jobs_malformed.txt:10: error: unterminated ' quote in command"
expect "jobs_malformed.txt:11: error: missing ','"
expect "no jobs were started"; expect_no "Starting Job"; end

begin "9a. dispatch file with only comments/blank lines"
run t09_empty 2 tests/jobs_empty.txt; expect "contains no jobs"; end
begin "9b. zero-byte dispatch file"
: >"$TMPDIR_T/zero.txt"
run t09_zero 2 "$TMPDIR_T/zero.txt"; expect "contains no jobs"; end

begin "10. nonexistent dispatch file"
run t10_nofile 2 tests/does_not_exist.txt
expect "cannot open dispatch file 'tests/does_not_exist.txt': No such file or directory"; end

begin "10b. unreadable path (a directory)"
run t10_dir 2 tests; expect "error:"; end

begin "14. decreasing arrival times rejected"
run t14_decreasing 2 tests/jobs_decreasing.txt
expect "jobs_decreasing.txt:4: error: arrival time 3 is earlier than 5 on line 3"
expect_no "Starting Job"; end

begin "Parser: CRLF line endings, commas inside a command"
printf '0,echo one,two\r\n0,0,2,64,0,0,0,0,  echo   spaced   out  \r\n' >"$TMPDIR_T/crlf.txt"
run p_crlf 0 "$TMPDIR_T/crlf.txt"
expect "one,two"; expect "spaced out"; expect_result 1 "completed"; end

begin "Sheet format: 8 fields with spaces, no command -> default sleepy"
printf '0, 0, 2, 64, 0, 0, 0, 0\n' >"$TMPDIR_T/nocmd.txt"
run p_nocmd 0 "$TMPDIR_T/nocmd.txt"
expect "[sleepy pid="; expect_times 1 0 2; expect_result 1 "budget expired: SIGINT"; end

begin "Bundled programs found without a path (0,sleepy 1)"
printf '0,sleepy 1\n' >"$TMPDIR_T/sleepy1.txt"
run p_sleepy 0 "$TMPDIR_T/sleepy1.txt"
expect "[sleepy pid="; expect_result 1 "completed (exit 0)"; end

begin "Parser: more than 63 arguments rejected"
{ printf '0,echo'; for i in $(seq 1 70); do printf ' a%d' "$i"; done; echo; } >"$TMPDIR_T/manyargs.txt"
run p_manyargs 2 "$TMPDIR_T/manyargs.txt"; expect "more than 63 arguments"; end

begin "Parser: very long line (20 000 chars) handled, no fixed buffer"
{ printf '0,echo '; head -c 20000 /dev/zero | tr '\0' 'x'; echo; } >"$TMPDIR_T/long.txt"
run p_long 0 "$TMPDIR_T/long.txt"; expect_result 1 "completed (exit 0)"; end

# ------------------------------------------------------------ signals ----
begin "12. Ctrl+C (SIGINT to dispatcher) stops and reaps the child"
LOG="$LOGDIR/t12_interrupt.log"
timeout 90 "$BM" tests/jobs_interrupt.txt >"$LOG" 2>&1 &
BG=$!
sleep 2.5
kill -INT "$BG"
wait "$BG"; RC=$?
[[ $RC -eq 130 ]] || bad "exit code $RC, expected 130"
expect "Received SIGINT. Shutting down safely."
expect_result 1 "interrupted (Ctrl+C)"; expect_result 2 "not started"; end

begin "12b. Ctrl+C while the child ignores SIGINT -> SIGKILL used"
echo '0,0,30,64,0,0,0,0,./build/test_worker 20 --ignore-sigint --name deaf' >"$TMPDIR_T/deaf.txt"
LOG="$LOGDIR/t12b_interrupt_ignore.log"
timeout 90 "$BM" "$TMPDIR_T/deaf.txt" >"$LOG" 2>&1 &
BG=$!
sleep 1.5
kill -INT "$BG"
wait "$BG"; RC=$?
[[ $RC -eq 130 ]] || bad "exit code $RC, expected 130"
expect "sending SIGKILL (last resort)"; expect_result 1 "interrupted (Ctrl+C)"; end

begin "12c. SIGTERM to dispatcher also shuts down cleanly (exit 143)"
LOG="$LOGDIR/t12c_sigterm.log"
timeout 90 "$BM" tests/jobs_interrupt.txt >"$LOG" 2>&1 &
BG=$!
sleep 1.5
kill -TERM "$BG"
wait "$BG"; RC=$?
[[ $RC -eq 143 ]] || bad "exit code $RC, expected 143"
expect "Received SIGTERM"; end

# ------------------------------------------------ specification examples ----
if [[ ${QUICK:-0} != 1 ]]; then
    begin "Project sheet process list, exactly as given (20 s)"
    run sheet_processlist 0 tests/jobs_sheet_processlist.txt
    expect_times 1 0 3; expect_times 2 3 9; expect_times 3 9 13
    expect_times 4 13 18; expect_times 5 18 20
    for j in 1 2 3 4 5; do expect_result $j "budget expired: SIGINT"; done
    expect "Average waiting time: 4.60 ticks.  Average turnaround: 8.60 ticks."; end

    begin "Project sheet command list, 0,sleepy 3 ... exactly as given (20 s)"
    run sheet_sleepy 0 tests/jobs_sheet_sleepy.txt
    expect_times 1 0 3; expect_times 2 3 9; expect_times 3 9 13
    expect_times 4 13 18; expect_times 5 18 20
    for j in 1 2 3 4 5; do expect_result $j "completed (exit 0)"; done; end

    begin "Spec example, simple format: textbook FCFS schedule (20 s)"
    run spec_simple 0 tests/jobs_simple_format.txt
    expect_times 1 0 3; expect_times 2 3 9; expect_times 3 9 13
    expect_times 4 13 18; expect_times 5 18 20
    expect "Average waiting time: 4.60 ticks.  Average turnaround: 8.60 ticks."; end

    begin "Spec example, full 8-field format: same schedule (20 s)"
    run spec_basic 0 tests/jobs_basic.txt
    expect_times 1 0 3; expect_times 2 3 9; expect_times 3 9 13
    expect_times 4 13 18; expect_times 5 18 20; end
else
    echo "(QUICK=1: skipped the four 20-second specification examples)"
fi

echo "========================================"
echo "Passed: $PASS   Failed: $FAIL"
if (( FAIL > 0 )); then
    printf '  failed: %s\n' "${FAILED_NAMES[@]}"
    exit 1
fi
exit 0
