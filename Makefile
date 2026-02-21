CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude -g -O2
SRCS = src/main.c src/util.c src/lexer.c src/parser.c src/ast_dump.c src/codegen.c
OBJS = $(SRCS:.c=.o)
TARGET = naznc

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c include/compiler.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(TARGET)

test: $(TARGET) tests/hello.src
	./$(TARGET) -dump-ast tests/hello.src
	./$(TARGET) -c -o tests/hello tests/hello.src
	./tests/hello

.PHONY: all clean test
