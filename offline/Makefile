CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE

all: game cheat

game: game.c
	$(CC) $(CFLAGS) -o $@ $< -lncursesw

cheat: cheat.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f game cheat

.PHONY: all clean
