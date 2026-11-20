CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -O0

.PHONY: all clean

all: webserver

webserver: server.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f webserver
