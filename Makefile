CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pthread
LDFLAGS = -pthread

all: logpulse loggen

logpulse: src/logpulse.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

loggen: src/loggen.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

gen: loggen
	./loggen -n 1000000 -o access.log

run: logpulse
	./logpulse -m lote -t 4 access.log

clean:
	rm -f logpulse loggen access.log

.PHONY: all clean run gen