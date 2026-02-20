CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc -Isrc/tls/bearssl
LDFLAGS = -lm -lpthread

# Feature flags (all enabled by default)
XSC_ENABLE_VM      ?= 1
XSC_ENABLE_JIT     ?= 1
XSC_ENABLE_PLUGINS ?= 1
XSC_ENABLE_SANDBOX ?= 1
XSC_ENABLE_TRACER  ?= 1
XSC_ENABLE_LSP     ?= 1
XSC_ENABLE_DAP     ?= 1

# Platform detection
STRIP_FLAG = -s
ifeq ($(OS),Windows_NT)
  CFLAGS  += -D__USE_MINGW_ANSI_STDIO=1 -D_WIN32
  LDFLAGS += -lws2_32 -lpsapi -static
  TARGET  = xs.exe
else
  UNAME := $(shell uname -s)
  ifeq ($(UNAME),Linux)
    # Skip host multiarch paths when cross-compiling to a non-x86_64 target;
    # the cross toolchain already knows its own sysroot / include layout.
    HOST_ARCH := $(shell uname -m)
    TARGET_ARCH := $(shell $(CC) -dumpmachine 2>/dev/null | sed 's/-.*//')
    ifeq ($(HOST_ARCH),$(TARGET_ARCH))
      ifneq ($(wildcard /usr/include/x86_64-linux-gnu),)
        CFLAGS += -isystem /usr/include/x86_64-linux-gnu
      endif
      ifneq ($(wildcard /usr/include),)
        CFLAGS += -isystem /usr/include
      endif
    endif
    LDFLAGS += -ldl
  endif
  ifeq ($(UNAME),Darwin)
    # macOS: no -ldl needed (dlopen is in libc); Apple ld rejects -s
    CFLAGS += -Wno-unused-command-line-argument
    STRIP_FLAG =
  endif
  TARGET = xs
endif

# Core sources (always built)
CORE_SRCS = src/core/value.c \
            src/core/ast.c \
            src/core/lexer.c \
            src/core/env.c \
            src/core/parser.c \
            src/core/xs_bigint.c \
            src/core/gc.c \
            src/core/utf8.c \
            src/core/strbuf.c \
            src/core/limits.c

COMPILER_SRCS = src/compiler/match_compiler.c

RUNTIME_SRCS = src/runtime/interp.c \
               src/runtime/builtins.c \
               src/runtime/builtins_math.c \
               src/runtime/builtins_time.c \
               src/runtime/builtins_string.c \
               src/runtime/builtins_path.c \
               src/runtime/builtins_base64.c \
               src/runtime/builtins_hash.c \
               src/runtime/builtins_uuid.c \
               src/runtime/builtins_random.c \
               src/runtime/builtins_json.c \
               src/runtime/builtins_csv.c \
               src/runtime/builtins_url.c \
               src/runtime/builtins_toml.c \
               src/runtime/builtins_log.c \
               src/runtime/builtins_fmt.c \
               src/runtime/builtins_test.c \
               src/runtime/builtins_re.c \
               src/runtime/builtins_collections.c \
               src/runtime/builtins_process.c \
               src/runtime/builtins_io.c \
               src/runtime/builtins_os.c \
               src/runtime/builtins_async.c \
               src/runtime/builtins_net.c \
               src/runtime/builtins_crypto.c \
               src/runtime/builtins_thread.c \
               src/runtime/builtins_buf.c \
               src/runtime/builtins_encode.c \
               src/runtime/builtins_db.c \
               src/runtime/builtins_cli.c \
               src/runtime/builtins_ffi.c \
               src/runtime/builtins_reflect.c \
               src/runtime/builtins_gc.c \
               src/runtime/builtins_reactive.c \
               src/runtime/builtins_http.c \
               src/runtime/builtins_fs.c \
               src/runtime/error.c \
               src/runtime/stdlib.c \
               src/runtime/scheduler.c \
               src/runtime/event_loop.c \
               src/runtime/concurrent.c

REPL_SRCS = src/repl/repl.c

LINT_SRCS = src/lint/lint.c

TYPES_EXTRA_SRCS = src/types/inference.c

EMBED_SRCS = src/xs_embed.c

DIAG_SRCS = src/diagnostic/diagnostic.c \
            src/diagnostic/render.c \
            src/diagnostic/colorize.c \
            src/diagnostic/explain.c

# Always-compiled tool sources (main.c references these unconditionally)
TOOL_SRCS = src/fmt/fmt.c \
            src/doc/docgen.c \
            src/pkg/pkg.c \
            src/profiler/profiler.c \
            src/coverage/coverage.c \
            src/optimizer/optimizer.c \
            src/optimizer/ssa.c \
            src/optimizer/inline_cache.c \
            src/ir/ir.c

DB_SRCS = src/db/xsdb.c

NET_SRCS = src/net/http_server.c

REGEX_SRCS = src/core/regex.c

MAIN_SRCS = src/main.c

CHECKER_SRCS = src/types/checker.c

MSGPACK_SRCS = src/core/msgpack.c

ASYNC_SRCS = src/runtime/async.c

SEMA_SRCS = src/types/types.c \
            src/semantic/exhaust.c \
            src/semantic/symtable.c \
            src/semantic/resolve.c \
            src/semantic/typecheck.c \
            src/semantic/cache.c \
            src/semantic/sema.c

TLS_SRCS = $(wildcard src/tls/*.c) $(wildcard src/tls/bearssl/**/*.c) $(wildcard src/tls/bearssl/*.c)

SRCS = $(CORE_SRCS) $(COMPILER_SRCS) $(RUNTIME_SRCS) $(REPL_SRCS) $(LINT_SRCS) $(TYPES_EXTRA_SRCS) $(EMBED_SRCS) $(DIAG_SRCS) $(TOOL_SRCS) $(MAIN_SRCS) $(SEMA_SRCS) $(TLS_SRCS) $(DB_SRCS) $(NET_SRCS) $(REGEX_SRCS) $(CHECKER_SRCS) $(MSGPACK_SRCS) $(ASYNC_SRCS)

# Conditional sources
ifeq ($(XSC_ENABLE_VM),1)
  CFLAGS += -DXSC_ENABLE_VM
  SRCS += $(wildcard src/vm/*.c)
endif

ifeq ($(XSC_ENABLE_JIT),1)
  CFLAGS += -DXSC_ENABLE_JIT
  SRCS += $(wildcard src/jit/*.c)
endif

ifeq ($(XSC_ENABLE_PLUGINS),1)
  CFLAGS += -DXSC_ENABLE_PLUGINS
  SRCS += $(wildcard src/plugins/*.c)
endif

ifeq ($(XSC_ENABLE_SANDBOX),1)
  CFLAGS += -DXSC_ENABLE_SANDBOX
endif

ifeq ($(XSC_ENABLE_TRACER),1)
  CFLAGS += -DXSC_ENABLE_TRACER
  SRCS += $(wildcard src/tracer/*.c)
endif

ifeq ($(XSC_ENABLE_LSP),1)
  CFLAGS += -DXSC_ENABLE_LSP
  SRCS += $(wildcard src/lsp/*.c)
endif

ifeq ($(XSC_ENABLE_DAP),1)
  CFLAGS += -DXSC_ENABLE_DAP
  SRCS += $(wildcard src/dap/*.c)
endif

XSC_ENABLE_EFFECTS  ?= 1
XSC_ENABLE_TRANSPILER ?= 1
XSC_ENABLE_FMT      ?= 1
XSC_ENABLE_PKG      ?= 1
XSC_ENABLE_PROFILER ?= 1
XSC_ENABLE_COVERAGE ?= 1
XSC_ENABLE_DOC      ?= 1

ifeq ($(XSC_ENABLE_EFFECTS),1)
  CFLAGS += -DXSC_ENABLE_EFFECTS
  SRCS += $(wildcard src/effects/*.c)
endif

ifeq ($(XSC_ENABLE_TRANSPILER),1)
  CFLAGS += -DXSC_ENABLE_TRANSPILER
  SRCS += $(wildcard src/transpiler/*.c)
endif

ifeq ($(XSC_ENABLE_FMT),1)
  CFLAGS += -DXSC_ENABLE_FMT
endif

ifeq ($(XSC_ENABLE_PKG),1)
  CFLAGS += -DXSC_ENABLE_PKG
endif

ifeq ($(XSC_ENABLE_PROFILER),1)
  CFLAGS += -DXSC_ENABLE_PROFILER
endif

ifeq ($(XSC_ENABLE_COVERAGE),1)
  CFLAGS += -DXSC_ENABLE_COVERAGE
endif

ifeq ($(XSC_ENABLE_DOC),1)
  CFLAGS += -DXSC_ENABLE_DOC
endif

OBJS = $(SRCS:.c=.o)

# Targets
.PHONY: all clean debug release test test-unit test-e2e test-negative test-property test-golden test-regression test-conformance test-all install wasm

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# BearSSL is upstream constant-time crypto. Under -flto the optimiser
# inlines the interleave helpers and loses track of the caller's
# scratch-array initialisation, producing spurious maybe-uninitialized
# warnings. The code is fine; silence the bearssl directory only.
src/tls/bearssl/%.o: src/tls/bearssl/%.c
	$(CC) $(CFLAGS) -Wno-maybe-uninitialized -c -o $@ $<

debug: CFLAGS = -g -O0 -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc -Isrc/tls/bearssl \
                -fsanitize=address -fsanitize=undefined -DDEBUG \
                $(foreach f,VM JIT PLUGINS SANDBOX TRACER LSP DAP EFFECTS TRANSPILER FMT PKG PROFILER COVERAGE DOC,-DXSC_ENABLE_$(f))
debug: LDFLAGS += -fsanitize=address -fsanitize=undefined
debug: clean $(TARGET)

release: CFLAGS = -O3 -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc -Isrc/tls/bearssl \
                  -DNDEBUG -flto \
                  $(foreach f,VM JIT PLUGINS SANDBOX TRACER LSP DAP EFFECTS TRANSPILER FMT PKG PROFILER COVERAGE DOC,-DXSC_ENABLE_$(f))
# LTO re-runs maybe-uninitialized analysis across TUs; the bearssl
# AES path trips a false positive once the interleave helpers get
# inlined. The compile-time suppression is in the per-file rule above;
# mirror it at link time so LTO stays quiet too.
release: LDFLAGS += -flto $(STRIP_FLAG) -Wno-maybe-uninitialized
release: clean $(TARGET)

test: $(TARGET)
	@bash tests/run-all.sh

# Unit tests: isolated C tests that exercise compiler internals (lexer,
# parser, sema, vm) without going through the full `xs` binary. Each
# tests/unit/*_test.c is a self-contained program.
UNIT_CFLAGS = -O0 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -std=c11 \
              -Isrc -Isrc/tls/bearssl -Itests/unit \
              $(foreach f,VM PLUGINS SANDBOX EFFECTS TRANSPILER FMT PKG DOC,-DXSC_ENABLE_$(f))
UNIT_LINK_SRCS = $(CORE_SRCS) $(COMPILER_SRCS) $(DIAG_SRCS) $(SEMA_SRCS) \
                 $(MSGPACK_SRCS) $(REGEX_SRCS) tests/unit/stubs.c
UNIT_LDFLAGS = -lm -lpthread
ifeq ($(OS),Windows_NT)
  UNIT_LDFLAGS += -lws2_32
else ifeq ($(UNAME),Linux)
  UNIT_LDFLAGS += -ldl
endif

UNIT_TESTS = tests/unit/lexer_test tests/unit/parser_test tests/unit/sema_test \
             tests/unit/value_test tests/unit/gc_test tests/unit/utf8_test \
             tests/unit/bigint_test tests/unit/regex_test tests/unit/msgpack_test \
             tests/unit/strbuf_test tests/unit/limits_test

tests/unit/%_test: tests/unit/%_test.c $(UNIT_LINK_SRCS)
	$(CC) $(UNIT_CFLAGS) -o $@ $< $(UNIT_LINK_SRCS) $(UNIT_LDFLAGS)

test-unit: $(UNIT_TESTS)
	@failed=0; for t in $(UNIT_TESTS); do \
	    ./$$t || failed=1; \
	done; \
	if [ $$failed -ne 0 ]; then echo "[unit] FAILED"; exit 1; fi

test-e2e: $(TARGET)
	@bash tests/run.sh

test-negative: $(TARGET)
	@bash tests/negative/run.sh

test-property: $(TARGET)
	@bash tests/property/run.sh

test-golden: $(TARGET)
	@bash tests/golden/run.sh

test-regression: $(TARGET)
	@for f in tests/regression/*.xs; do \
	    [ -f "$$f" ] || continue; \
	    ./$(TARGET) "$$f" >/dev/null || { echo "FAIL $$f"; exit 1; }; \
	done; echo "[regression] ok"

test-conformance: $(TARGET)
	@for f in tests/conformance/*.xs; do \
	    [ -f "$$f" ] || continue; \
	    ./$(TARGET) "$$f" >/dev/null || { echo "FAIL $$f"; exit 1; }; \
	done; echo "[conformance] ok"

# Full 7-layer test architecture. See tests/run-all.sh for details.
test-all: $(TARGET)
	@bash tests/run-all.sh

# Cross-backend diff tests: run a program on the interpreter, VM, transpiled
# C, and transpiled JS, and assert all four produce the same stdout. Not part
# of `make test` because several known backend gaps still show up here; use
# it when you change a backend to be sure you haven't diverged.
test-diff: $(TARGET)
	@bash tests/test_transpiler_diff.sh

# Run the full test suite against an AddressSanitizer + UBSan build.
# Catches memory and undefined-behavior bugs the default build can miss.
test-asan: debug
	@bash tests/run.sh

# Focused AddressSanitizer-only build. Lower overhead than `debug` when
# the goal is just catching out-of-bounds / use-after-free.
asan: CFLAGS = -g -O1 -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc -Isrc/tls/bearssl \
               -fsanitize=address -fno-omit-frame-pointer \
               $(foreach f,VM JIT PLUGINS SANDBOX TRACER LSP DAP EFFECTS TRANSPILER FMT PKG PROFILER COVERAGE DOC,-DXSC_ENABLE_$(f))
asan: LDFLAGS += -fsanitize=address
asan: clean $(TARGET)

ubsan: CFLAGS = -g -O1 -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc -Isrc/tls/bearssl \
                -fsanitize=undefined -fno-omit-frame-pointer \
                $(foreach f,VM JIT PLUGINS SANDBOX TRACER LSP DAP EFFECTS TRANSPILER FMT PKG PROFILER COVERAGE DOC,-DXSC_ENABLE_$(f))
ubsan: LDFLAGS += -fsanitize=undefined
ubsan: clean $(TARGET)

# libFuzzer entrypoint against the parser. Needs clang with -fsanitize=fuzzer.
# Build corpus by pointing fuzz_parser at tests/ and examples/.
FUZZ_CC ?= clang
fuzz-parser: tests/fuzz/fuzz_parser.c tests/fuzz/stubs.c
	$(FUZZ_CC) -g -O1 -fsanitize=fuzzer,address,undefined -std=c11 \
	    -Isrc -Isrc/tls/bearssl \
	    $(foreach f,VM PLUGINS SANDBOX EFFECTS TRANSPILER FMT PKG DOC,-DXSC_ENABLE_$(f)) \
	    -o fuzz_parser tests/fuzz/fuzz_parser.c tests/fuzz/stubs.c \
	    $(CORE_SRCS) $(COMPILER_SRCS) $(DIAG_SRCS) \
	    -lm -lpthread -ldl

install: release
	install -m 755 $(TARGET) /usr/local/bin/xs

WASM_SRCS = src/wasm_main.c \
            $(CORE_SRCS) $(RUNTIME_SRCS) $(TYPES_EXTRA_SRCS) $(EMBED_SRCS) \
            $(DIAG_SRCS) $(SEMA_SRCS) \
            $(MSGPACK_SRCS) $(ASYNC_SRCS) $(REGEX_SRCS) \
            $(COMPILER_SRCS) $(CHECKER_SRCS) $(DB_SRCS) $(NET_SRCS) \
            src/fmt/fmt.c src/doc/docgen.c src/lint/lint.c \
            src/pkg/pkg.c src/coverage/coverage.c \
            src/optimizer/optimizer.c src/optimizer/ssa.c src/optimizer/inline_cache.c \
            src/ir/ir.c src/profiler/profiler.c \
            $(wildcard src/vm/*.c) \
            $(wildcard src/effects/*.c) \
            $(wildcard src/transpiler/*.c) \
            $(wildcard src/plugins/*.c) \
            $(wildcard src/tracer/*.c) \
            src/wasm_stubs.c

WASI_SDK ?= /opt/wasi-sdk
WASI_CC = $(WASI_SDK)/bin/clang
WASI_SYSROOT = $(WASI_SDK)/share/wasi-sysroot

WASM_FLAGS = -O2 -std=c11 -Isrc \
             --target=wasm32-wasi \
             --sysroot=$(WASI_SYSROOT) \
             -mllvm -wasm-enable-sjlj \
             -D_WASI_EMULATED_SIGNAL \
             -D_WASI_EMULATED_PTHREAD \
             -DXS_WASM=1 \
             -DXSC_ENABLE_VM -DXSC_ENABLE_EFFECTS -DXSC_ENABLE_TRANSPILER \
             -DXSC_ENABLE_FMT -DXSC_ENABLE_DOC -DXSC_ENABLE_PKG \
             -DXSC_ENABLE_PLUGINS -DXSC_ENABLE_SANDBOX \
             -DXSC_ENABLE_PROFILER -DXSC_ENABLE_COVERAGE \
             -DXSC_ENABLE_TRACER \
             -Wno-incompatible-library-redeclaration \
             -Wno-implicit-function-declaration \
             -Wno-int-conversion

wasm:
	$(WASI_CC) $(WASM_FLAGS) -lwasi-emulated-signal -lwasi-emulated-pthread -o xs.wasm $(WASM_SRCS)
	@echo "built xs.wasm"

clean:
	rm -f $(OBJS) $(TARGET)
	find src -name '*.o' -delete
