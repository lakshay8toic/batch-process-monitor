# Makefile for the Batch Process Monitor (FCFS dispatcher)
#
#   make            build build/batch_monitor and build/test_worker
#   make run        run the specification's example job list
#   make test       run the automated test suite (~75 s; QUICK=1 for ~35 s)
#   make memcheck   run under valgrind to check for leaks (optional)
#   make clean      delete build/

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -g
# The POSIX feature-test macro (_POSIX_C_SOURCE 200809L) is defined at the top
# of each .c file, so each file also compiles on its own with plain gcc.
BUILD   = build

MONITOR = $(BUILD)/batch_monitor
WORKER  = $(BUILD)/test_worker

.PHONY: all run test memcheck clean

all: $(MONITOR) $(WORKER)

$(BUILD):
	mkdir -p $(BUILD)

$(MONITOR): src/batch_monitor.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/batch_monitor.c

$(WORKER): tests/test_worker.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_worker.c

# Demo: the example job list from the project specification.
run: all
	./$(MONITOR) tests/jobs_basic.txt

# Automated test suite (real 1-second ticks, real child processes).
test: all
	bash tests/run_tests.sh

# Optional: check for memory leaks with valgrind (if installed).
memcheck: all
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=99 \
	    ./$(MONITOR) tests/jobs_error_cases.txt

clean:
	rm -rf $(BUILD)
