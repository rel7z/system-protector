CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local

protector: protector.c
	$(CC) $(CFLAGS) -o protector protector.c

install: protector
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 protector $(DESTDIR)$(PREFIX)/bin/protector

clean:
	rm -f protector

.PHONY: install clean
