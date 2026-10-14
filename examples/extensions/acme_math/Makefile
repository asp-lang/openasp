CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -Wpedantic

all: libacme_math.dylib

libacme_math.dylib: acme_math.c
	$(CC) $(CFLAGS) -fPIC -shared -I../../../include $< -o $@

clean:
	rm -f libacme_math.dylib
