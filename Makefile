CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2 -g
TARGET = rcc

SRCS = src/main.c src/lexer.c src/preprocess.c src/parser.c src/type.c src/codegen.c src/opt.c src/alloc.c src/unicode.c
OBJS = $(SRCS:.c=.o)

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
INCDIR = $(PREFIX)/include/rcc

# Build-time include directory: absolute path to the source include/ dir.
# Override this when installing to a different prefix.
# On native Windows builds, default to the standard install location.
ifeq ($(OS),Windows_NT)
RCC_INCDIR ?= C:/Program Files/rcc/include
DEF_INCDIR='-DRCC_INCDIR="$(RCC_INCDIR)"'
TARGET = rcc.exe
else
ifeq ($(CC),x86_64-w64-mingw32-gcc)
TARGET = rcc.exe
else
RCC_INCDIR ?= $(shell cd include && pwd)
DEF_INCDIR='-DRCC_INCDIR="$(RCC_INCDIR)"'
endif
endif

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/sysinc_paths.h:
	./tools/get-sysinc-paths.sh $(CC) > $@

src/main.o: src/main.c src/sysinc_paths.h
	$(CC) $(CFLAGS) -c $< -o $@ -DGCC=\"$(CC)\" $(DEF_INCDIR)
src/preprocess.o: src/preprocess.c src/sysinc_paths.h
	$(CC) $(CFLAGS) -c $< -o $@ $(DEF_INCDIR)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(OS),Windows_NT)
TEST_RUNNER = powershell -ExecutionPolicy Bypass -File run_tcc_suite.ps1
BENCH_RUNNER = powershell -ExecutionPolicy Bypass -File bench/run_bench.ps1 ./$(TARGET)
else
TEST_RUNNER = ./run_tcc_suite.sh
BENCH_RUNNER = ./bench/run_bench.sh ./$(TARGET)
endif

test check: $(TARGET)
	@$(TEST_RUNNER)

lint:
	if command -v prek; then prek run -a; \
        elif command -v pre-commit; then pre-commit run --all-files; fi

bench: $(TARGET)
	@$(BENCH_RUNNER)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(INCDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/
	install -m 644 include/* $(DESTDIR)$(INCDIR)/
	# Rebuild with the installed include path so rcc finds its headers
	# without needing -I after installation.
	$(MAKE) clean
	$(MAKE) RCC_INCDIR=$(DESTDIR)$(INCDIR)

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET).exe src/sysinc_paths.h

TAGS: $(SRCS) src/rcc.h
	etags -a --language=c src/*.c src/*.h

.PHONY: clean test check bench install
