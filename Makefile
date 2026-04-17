CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2 -g
TARGET = rcc

SRCS = src/main.c src/lexer.c src/preprocess.c src/parser.c src/type.c src/codegen.c src/opt.c src/alloc.c
OBJS = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(OS),Windows_NT)
TEST_RUNNER = powershell -ExecutionPolicy Bypass -File run_tcc_suite.ps1
else
TEST_RUNNER = ./run_tcc_suite.sh ./$(TARGET)
endif

test check: $(TARGET)
	@$(TEST_RUNNER)

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET).exe

.PHONY: clean test check
