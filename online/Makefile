CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE

all: server cheat

server: server.c
	$(CC) $(CFLAGS) -o $@ $<

cheat: cheat.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f server cheat

.PHONY: all clean
