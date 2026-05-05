# SPDX-License-Identifier: LGPL-2.1-or-later
CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -O3 -g
TARGET = rcc
MINGW_O =
OBJ_EXT = .o

# Detect clang vs gcc
IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -c clang)

ifeq ($(IS_CLANG),0)
CFLAGS += -flto=auto
else
CFLAGS += -flto=thin
# Probe for LLVM LTO plugin (needed when linking with ld.bfd/ld.gold).
# Try llvm-config-<major> first (avoids broken alternatives symlinks), then llvm-config.
#LLVM_LIBDIR := $(shell \
#    major=$$($(CC) --version 2>/dev/null | grep -oE 'clang version [0-9]+' | grep -oE '[0-9]+'); \
#    for cmd in "llvm-config-$$major" llvm-config; do \
#        d=$$($$cmd --libdir 2>/dev/null) && test -n "$$d" && echo "$$d" && break; \
#    done)
#ifneq ($(LLVM_LIBDIR),)
#LTO_PLUGIN := $(LLVM_LIBDIR)/LLVMgold.so
#ifneq ($(wildcard $(LTO_PLUGIN)),)
#LDFLAGS += -Wl,-plugin,$(LTO_PLUGIN)
#endif
#endif
endif

SRCS = src/main.c src/lexer.c src/preprocess.c src/parser.c src/type.c src/codegen.c src/opt.c src/alloc.c src/unicode.c
OBJS = $(SRCS:.c=$(OBJ_EXT))

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
INCDIR = $(PREFIX)/include/rcc
LIBDIR = $(PREFIX)/lib/rcc
DOCDIR = $(PREFIX)/share/doc/rcc

# Build-time include directory: absolute path to the source include/ dir.
# Override this when installing to a different prefix.
RCC_INCDIR ?= $(CURDIR)/include
# On native Windows builds, default to the standard install location.
ifeq ($(OS),Windows_NT)
TARGET = rcc.exe
MINGW_O = lib/mingw$(OBJ_EXT)
OBJ_EXT = .obj
PREFIX ?= C:/Program Files/rcc
BINDIR = $(PREFIX)
INCDIR = $(PREFIX)/include
LIBDIR = $(PREFIX)/lib
DOCDIR = $(PREFIX)/doc
OBJS = $(SRCS:.c=$(OBJ_EXT))
else
ifeq ($(CC),x86_64-w64-mingw32-gcc)
TARGET = rcc.exe
MINGW_O = lib/mingw$(OBJ_EXT)
OBJ_EXT = .obj
OBJS = $(SRCS:.c=$(OBJ_EXT))
endif
ifeq ($(CC),aarch64-linux-gnu-gcc)
TARGET = rcc-arm64
OBJ_EXT = .arm64.o
OBJS = $(SRCS:.c=$(OBJ_EXT))
ARM64_SYSROOT := $(shell $(CC) -print-sysroot 2>/dev/null)
ifneq ($(ARM64_SYSROOT),/)
ifeq ($(shell test -d "$(ARM64_SYSROOT)/usr/include" && echo yes),)
ARM64_SYSROOT := /usr/aarch64-redhat-linux/sys-root/fc43
endif
CFLAGS += --sysroot=$(ARM64_SYSROOT)
endif
endif
endif
# Native Linux builds: optimize for the host CPU
ifeq ($(shell uname -s),Linux)
ifeq ($(CC),gcc)
CFLAGS += -march=native
endif
ifneq ($(IS_CLANG),0)
CFLAGS += -march=native
endif
endif
DEF_INCDIR = -DRCC_INCDIR='"$(RCC_INCDIR)"'
VERSION ?= $(shell git describe --long --tags --always 2>/dev/null || echo "v1.2-dev")
MACHINE ?= $(shell $(CC) -dumpmachine 2>/dev/null || echo "unknown")

ifneq ($(findstring apple,$(MACHINE)),)
DARWIN_O = lib/darwin.o
TARGET_DEPS = $(OBJS) $(DARWIN_O)
else
TARGET_DEPS = $(OBJS) $(MINGW_O)
endif

$(TARGET): $(TARGET_DEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

src/sysinc_paths.h:
	RCC_CC="$(CC)"; \
	if [ "$(CC)" = "aarch64-linux-gnu-gcc" ] || [ -n "$(ARM64_SYSROOT)" ]; then \
		./tools/get-sysinc-paths.sh "$(CC) --sysroot=$(ARM64_SYSROOT)" > $@; \
	else \
		./tools/get-sysinc-paths.sh $(CC) > $@; \
	fi

src/gcc_predefined.h:
	$(CC) -dM -E - < /dev/null | awk -f tools/get-gcc-predefined.awk > $@

$(DARWIN_O): lib/darwin.c
	$(CC) $(CFLAGS) -c $< -o $@
$(MINGW_O): lib/mingw.c
	$(CC) $(CFLAGS) -c $< -o $@
src/main$(OBJ_EXT): src/main.c src/sysinc_paths.h
	$(CC) $(CFLAGS) -c $< -o $@ -DGCC=\"$(CC)\" $(DEF_INCDIR) -DVERSION=\"$(VERSION)\" -DMACHINE=\"$(MACHINE)\"
src/preprocess$(OBJ_EXT): src/preprocess.c src/sysinc_paths.h src/gcc_predefined.h
	$(CC) $(CFLAGS) -c $< -o $@ $(DEF_INCDIR)
%$(OBJ_EXT): %.c
	$(CC) $(CFLAGS) -c $< -o $@

compile_commands.json: $(SRCS)
	$(MAKE) clean
	bear -- make

# Profile build: rcc compiled with -pg for gprof analysis
rcc_prof: CFLAGS += -pg
rcc_prof: $(SRCS) src/rcc.h src/sysinc_paths.h src/gcc_predefined.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) -DGCC=\"$(CC)\" $(DEF_INCDIR) -DVERSION=\"$(VERSION)\" -DMACHINE=\"$(MACHINE)\" -lm

# Run profile: compile a decent-sized file to generate gmon.out
prof: rcc_prof
	./rcc_prof -E bench/bench.c > /dev/null
	gprof ./rcc_prof gmon.out > gprof.txt
	@echo "Profile written to gprof.txt"
	@head -40 gprof.txt

ifeq ($(OS),Windows_NT)
TEST_RUNNER = powershell -ExecutionPolicy Bypass -File run_tcc_suite.ps1 -O1
BENCH_RUNNER = powershell -ExecutionPolicy Bypass -File bench/run_bench.ps1 ./$(TARGET)
else
TEST_RUNNER = ./run_tcc_suite.sh "" "" -O1 && ./run-c-testsuite.sh && test/compliance/run.sh
BENCH_RUNNER = ./bench/run_bench.sh ./$(TARGET)
endif

test check: $(TARGET)
	@$(TEST_RUNNER)

test-all: $(TARGET)
	$(MAKE) clean
	$(MAKE)
	@$(TEST_RUNNER)
	./mingw-test.sh
	./arm64-test.sh
	./darwin-test.sh
	test/torture/run.sh

lint:
	if command -v prek; then prek run -a; \
        elif command -v pre-commit; then pre-commit run --all-files; fi

bench: $(TARGET)
	@$(BENCH_RUNNER)

# Rebuild with the installed include path so rcc finds its headers
# without needing -I after installation.
install: $(TARGET)
	$(MAKE) clean
	$(MAKE) RCC_INCDIR="$(INCDIR)"
ifeq ($(OS),Windows_NT)
	install -d "$(if $(DESTDIR),$(DESTDIR)$(subst C:,,$(BINDIR)),$(BINDIR))" "$(if $(DESTDIR),$(DESTDIR)$(subst C:,,$(INCDIR)),$(INCDIR))" "$(if $(DESTDIR),$(DESTDIR)$(subst C:,,$(DOCDIR)),$(DOCDIR))"
	install -m 755 $(TARGET) "$(if $(DESTDIR),$(DESTDIR)$(subst C:,,$(BINDIR)),$(BINDIR))/"
	install -m 644 include/* "$(if $(DESTDIR),$(DESTDIR)$(subst C:,,$(INCDIR)),$(INCDIR))/"
	install -m 644 README.md tcc_test*.md LICENSE bench/bench_report*.md "$(if $(DESTDIR),$(DESTDIR)$(subst C:,,$(DOCDIR)),$(DOCDIR))/"
	if test -n "$(MINGW_O)"; then install -d "$(if $(DESTDIR),$(DESTDIR)$(subst C:,,$(LIBDIR)),$(LIBDIR))"; install -m 644 $(MINGW_O) "$(if $(DESTDIR),$(DESTDIR)$(subst C:,,$(LIBDIR)),$(LIBDIR))/"; fi
else
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(INCDIR)" "$(DESTDIR)$(DOCDIR)"
	install -m 755 $(TARGET) "$(DESTDIR)$(BINDIR)/"
	install -m 644 include/* "$(DESTDIR)$(INCDIR)/"
	install -m 644 README.md tcc_test*.md LICENSE bench/bench_report*.md "$(DESTDIR)$(DOCDIR)/"
	if test -n "$(MINGW_O)"; then install -d "$(DESTDIR)$(LIBDIR)"; install -m 644 $(MINGW_O) "$(DESTDIR)$(LIBDIR)/"; fi
endif

dist: $(TARGET)
	@echo make dist on $(OS)
	@rm -rf rcc-$(VERSION) || true
ifeq ($(OS),Windows_NT)
	$(MAKE) install DESTDIR="rcc-$(VERSION)" PREFIX=""
	cd rcc-$(VERSION) && powershell -command "Compress-Archive -Path * -DestinationPath ../rcc-$(VERSION).zip -Force"
	rm -rf rcc-$(VERSION)
	git checkout-index --prefix=rcc-$(VERSION)-src/ -a
	cd rcc-$(VERSION)-src && powershell -command "Compress-Archive -Path * -DestinationPath ../rcc-$(VERSION)-src.zip -Force"
else
	$(MAKE) install DESTDIR="rcc-$(VERSION)"
	tar cfz rcc-$(VERSION).tar.gz rcc-$(VERSION)
	tar cfJ rcc-$(VERSION).tar.xz rcc-$(VERSION)
	rm -rf rcc-$(VERSION)
	git checkout-index --prefix=rcc-$(VERSION)-src/ -a
	tar cfz rcc-$(VERSION)-src.tar.gz rcc-$(VERSION)-src
	tar cfJ rcc-$(VERSION)-src.tar.xz rcc-$(VERSION)-src
endif
	rm -rf rcc-$(VERSION)-src

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET).exe rcc_prof src/sysinc_paths.h src/gcc_predefined.h \
              fred.txt *.s qemu*.core src/*.obj src/*.darwin.o src/*.arm64.o lib/darwin.o lib/darwin$(OBJ_EXT)
	if command -v git > /dev/null 2>&1; then \
	  cd tinycc && git reset --hard && git clean -dxf tests/tests2; fi

TAGS: $(SRCS) src/rcc.h
	etags -a --language=c src/*.c src/*.h

.PHONY: clean test check lint bench install dist bench test-all prof
