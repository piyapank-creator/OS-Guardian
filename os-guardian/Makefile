CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread -Isrc
CORE    = src/deadlock.c src/proc_monitor.c

all: bin/osguardian bin/deadlock_demo bin/bench

bin/osguardian: src/main.c src/registry.c src/httpd.c $(CORE) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/deadlock_demo: tests/deadlock_demo.c | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/bench: tests/bench.c $(CORE) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin:
	mkdir -p bin

bench: bin/bench
	./bin/bench

clean:
	rm -rf bin results.csv
.PHONY: all bench clean
