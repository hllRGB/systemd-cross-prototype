CC = clang
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -static -fuse-ld=lld
CPPFLAGS ?= -Isrc

SRC = src/util.c src/config.c src/index.c src/plan.c src/main.c
OBJ = $(SRC:.c=.o)
BIN = systemd-cross

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

test: $(BIN)
	./tests/run.sh

.PHONY: all clean test
