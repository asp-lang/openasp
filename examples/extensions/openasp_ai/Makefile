CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -Wpedantic

all: libopenasp_ai.dylib

libopenasp_ai.dylib: openasp_ai.c
	$(CC) $(CFLAGS) -fPIC -shared -I../../../include $< -o $@

clean:
	rm -rf libopenasp_ai.dylib libopenasp_ai.dylib.dSYM
