CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2 -g
TARGET = rcc
MINGW_O =

SRCS = src/main.c src/lexer.c src/preprocess.c src/parser.c src/type.c src/codegen.c src/opt.c src/alloc.c src/unicode.c
OBJS = $(SRCS:.c=.o)

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
INCDIR = $(PREFIX)/include/rcc
LIBDIR = $(PREFIX)/lib/rcc

# Build-time include directory: absolute path to the source include/ dir.
# Override this when installing to a different prefix.
RCC_INCDIR ?= $(CURDIR)/include
# On native Windows builds, default to the standard install location.
ifeq ($(OS),Windows_NT)
TARGET = rcc.exe
MINGW_O = lib/mingw.o
PREFIX = C:/Program Files
BINDIR = C:/Program Files/rcc
INCDIR = C:/Program Files/rcc/include
LIBDIR = C:/Program Files/rcc/lib
else
ifeq ($(CC),x86_64-w64-mingw32-gcc)
TARGET = rcc.exe
MINGW_O = lib/mingw.o
endif
endif
DEF_INCDIR = -DRCC_INCDIR='"$(RCC_INCDIR)"'

$(TARGET): $(OBJS) $(MINGW_O)
	$(CC) $(CFLAGS) -o $@ $^

src/sysinc_paths.h:
	./tools/get-sysinc-paths.sh $(CC) > $@

$(MINGW_O): lib/mingw.c
	$(CC) $(CFLAGS) -c $< -o $@
src/main.o: src/main.c src/sysinc_paths.h
	$(CC) $(CFLAGS) -c $< -o $@ -DGCC=\"$(CC)\" $(DEF_INCDIR)
src/preprocess.o: src/preprocess.c src/sysinc_paths.h
	$(CC) $(CFLAGS) -c $< -o $@ $(DEF_INCDIR)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

compile_commands.json: $(SRCS)
	make clean
	bear -- make

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

# Rebuild with the installed include path so rcc finds its headers
# without needing -I after installation.
install: $(TARGET)
	$(MAKE) clean
	$(MAKE) RCC_INCDIR=$(DESTDIR)$(INCDIR)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(INCDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/
	install -m 644 include/* $(DESTDIR)$(INCDIR)/
	if test -n "$(MINGW_O)"; then install -d $(DESTDIR)$(LIBDIR); install -m 644 $(MINGW_O) $(DESTDIR)$(LIBDIR)/ ; fi

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET).exe src/sysinc_paths.h
	if command -v git > /dev/null 2>&1; then \
	  cd tinycc && git reset --hard && git clean -dxf tests/tests2; fi

TAGS: $(SRCS) src/rcc.h
	etags -a --language=c src/*.c src/*.h

.PHONY: clean test check bench install
