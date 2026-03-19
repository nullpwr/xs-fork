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
            src/core/gc_concurrent.c \
            src/core/utf8.c \
            src/core/strbuf.c \
            src/core/limits.c

COMPILER_SRCS = src/compiler/match_compiler.c

RUNTIME_SRCS = src/runtime/interp.c \
               src/runtime/interp_ast.c \
               src/runtime/interp_enum.c \
               src/runtime/interp_nodemap.c \
               src/runtime/interp_typecheck.c \
               src/runtime/interp_derive.c \
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
               src/runtime/builtins_tracing.c \
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
.PHONY: all clean debug release test test-unit test-e2e test-negative test-property test-golden test-regression test-conformance test-all install wasm wasm-browser bench bench-compare ios ios-device ios-sim ios-sim-arm64 android android-clean android-print-triple esp32 esp32-component

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

# Cross-runtime benchmark: times each bench under xs (interp/vm/jit)
# plus python3, node, and go (any missing runtime is skipped). Writes
# results.json with the best-of-N wall times; CI uses it to guard
# regressions against the committed baseline.
bench: $(TARGET)
	@bash benchmarks/run.sh

# Detect perf regressions against benchmarks/baseline.json. Exits non-zero
# if any numeric shrinks or grows by more than $$TOLERANCE (default 25%).
bench-compare: bench
	@python3 benchmarks/compare.py benchmarks/baseline.json benchmarks/results.json

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
             tests/unit/strbuf_test tests/unit/limits_test \
             tests/unit/bytecode_buf_test

tests/unit/%_test: tests/unit/%_test.c $(UNIT_LINK_SRCS)
	$(CC) $(UNIT_CFLAGS) -o $@ $< $(UNIT_LINK_SRCS) $(UNIT_LDFLAGS)

# bytecode_buf_test exercises just the .xsc reader, using the host xs
# binary as a fixture generator. Only needs core value + gc + utf8 +
# bytecode itself, no compiler / runtime / vm deps.
BYTECODE_BUF_TEST_SRCS = src/core/value.c src/core/gc.c src/core/utf8.c \
                         src/core/strbuf.c src/core/limits.c \
                         src/core/xs_bigint.c src/core/msgpack.c \
                         src/core/env.c src/vm/bytecode.c

tests/unit/bytecode_buf_test: tests/unit/bytecode_buf_test.c $(BYTECODE_BUF_TEST_SRCS)
	$(CC) $(UNIT_CFLAGS) -DXSC_ENABLE_VM -o $@ $< \
	    $(BYTECODE_BUF_TEST_SRCS) $(UNIT_LDFLAGS)

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

# Browser-targeted wasm: strip TLS (no sockets in wasi anyway), SSL engine,
# x509 chain verification, RSA and EC BearSSL backends, the bundled CA
# roots, http_server, and the transpiler/doc/lint/coverage tools that make
# no sense without a CLI. Keeps hash / mac / kdf / symcipher so the crypto
# module still works. Expects `wasm-opt` from binaryen; degrades gracefully
# if it isn't installed.
WASM_BROWSER_DROP = \
    src/tls/xs_tls.c src/tls/xs_ca_bundle.c \
    src/net/http_server.c \
    src/doc/docgen.c src/lint/lint.c \
    src/pkg/pkg.c \
    $(wildcard src/tls/bearssl/ssl/*.c) \
    $(wildcard src/tls/bearssl/x509/*.c) \
    $(wildcard src/tls/bearssl/rsa/*.c) \
    $(wildcard src/tls/bearssl/ec/*.c) \
    $(wildcard src/tls/bearssl/aead/*.c)

WASM_BROWSER_SRCS = $(filter-out $(WASM_BROWSER_DROP),$(WASM_SRCS))

WASM_BROWSER_FLAGS = -Oz -std=c11 -Isrc \
             --target=wasm32-wasi \
             --sysroot=$(WASI_SYSROOT) \
             -mllvm -wasm-enable-sjlj \
             -D_WASI_EMULATED_SIGNAL \
             -D_WASI_EMULATED_PTHREAD \
             -DXS_WASM=1 -DXS_BROWSER=1 \
             -DXSC_ENABLE_VM -DXSC_ENABLE_EFFECTS -DXSC_ENABLE_TRANSPILER \
             -DXSC_ENABLE_FMT -DXSC_ENABLE_PLUGINS -DXSC_ENABLE_SANDBOX \
             -DXSC_ENABLE_TRACER \
             -ffunction-sections -fdata-sections \
             -Wno-incompatible-library-redeclaration \
             -Wno-implicit-function-declaration \
             -Wno-int-conversion

wasm-browser:
	$(WASI_CC) $(WASM_BROWSER_FLAGS) \
	    -Wl,--gc-sections -Wl,--strip-all \
	    -lwasi-emulated-signal -lwasi-emulated-pthread \
	    -o xs-browser.wasm $(WASM_BROWSER_SRCS)
	@echo "built xs-browser.wasm ($$(wc -c < xs-browser.wasm) bytes)"
	@if command -v wasm-opt >/dev/null 2>&1; then \
	    wasm-opt -Oz --strip-debug --strip-producers \
	        xs-browser.wasm -o xs-browser.wasm.tmp && \
	    mv xs-browser.wasm.tmp xs-browser.wasm && \
	    echo "wasm-opt: $$(wc -c < xs-browser.wasm) bytes"; \
	else \
	    echo "wasm-opt not found; skipping post-link optimisation"; \
	fi

# ----------------------------------------------------------------------
# Mobile and embedded cross-compile targets.
#
# Apple's app-store policy bans W^X transitions, so iOS builds drop the
# JIT. ESP32 has no MMU and a few hundred KB of RAM, so its build also
# drops the JIT, FFI, transpilers, HTTP server, doc / lint / pkg tools,
# and the BearSSL X.509 verifier. Android keeps everything.
#
# The Raspberry Pi target is just a normal cross-compile of the full
# binary; see the README's `make release CC=aarch64-linux-gnu-gcc` line.
# ----------------------------------------------------------------------

# Sources shared by every mobile / embedded target. No CLI, no transpiler,
# no doc / lint / pkg, no http server. Keeps core, runtime, builtins, sema,
# vm, and the embed C entrypoint.
MOBILE_SRCS = $(CORE_SRCS) $(COMPILER_SRCS) $(RUNTIME_SRCS) \
              $(TYPES_EXTRA_SRCS) $(EMBED_SRCS) $(DIAG_SRCS) \
              $(SEMA_SRCS) $(MSGPACK_SRCS) $(ASYNC_SRCS) $(REGEX_SRCS) \
              $(CHECKER_SRCS) $(DB_SRCS) \
              src/profiler/profiler.c \
              src/optimizer/optimizer.c src/optimizer/ssa.c \
              src/optimizer/inline_cache.c src/ir/ir.c \
              $(wildcard src/vm/*.c) \
              $(wildcard src/effects/*.c) \
              $(wildcard src/tracer/*.c)

MOBILE_DEFINES = -DXSC_ENABLE_VM -DXSC_ENABLE_EFFECTS \
                 -DXSC_ENABLE_SANDBOX -DXSC_ENABLE_TRACER \
                 -DXSC_ENABLE_PROFILER -DXSC_ENABLE_COVERAGE

MOBILE_CFLAGS_BASE = -O2 -std=c11 -Isrc -Isrc/tls/bearssl \
                     -Wall -Wextra -Wno-unused-parameter \
                     -ffunction-sections -fdata-sections \
                     $(MOBILE_DEFINES) \
                     -Wno-incompatible-library-redeclaration \
                     -Wno-implicit-function-declaration \
                     -Wno-int-conversion

# ---- iOS ------------------------------------------------------------
# Builds a fat static archive xs-ios.a with arm64 (devices) and x86_64
# (simulator on Intel Macs). No JIT (Apple bans W^X without
# entitlements). Requires Xcode + xcrun on the host.
#
# For Apple Silicon simulator support, run ios-sim-arm64 separately and
# package the slices via `xcodebuild -create-xcframework`; lipo can't
# merge two archives that both contain an arm64 slice.
IOS_MIN_VERSION ?= 13.0
IOS_DEFINES = $(MOBILE_DEFINES) -DXS_IOS=1 -DXS_NO_JIT=1
IOS_OBJDIR = build/ios

ios: ios-device ios-sim
	lipo -create \
	    $(IOS_OBJDIR)/xs-ios-arm64.a \
	    $(IOS_OBJDIR)/xs-ios-sim-x86_64.a \
	    -output xs-ios.a
	@echo "built xs-ios.a ($$(wc -c < xs-ios.a) bytes)"

ios-device:
	@command -v xcrun >/dev/null 2>&1 || { echo "xcrun not found; iOS build needs Xcode"; exit 1; }
	mkdir -p $(IOS_OBJDIR)/arm64
	@for src in $(MOBILE_SRCS); do \
	    obj=$(IOS_OBJDIR)/arm64/$$(echo $$src | sed 's|/|_|g; s|\.c$$|.o|'); \
	    xcrun --sdk iphoneos clang $(MOBILE_CFLAGS_BASE) $(IOS_DEFINES) \
	        -arch arm64 -miphoneos-version-min=$(IOS_MIN_VERSION) \
	        -isysroot $$(xcrun --sdk iphoneos --show-sdk-path) \
	        -c $$src -o $$obj || exit 1; \
	done
	xcrun libtool -static -o $(IOS_OBJDIR)/xs-ios-arm64.a $(IOS_OBJDIR)/arm64/*.o

ios-sim:
	@command -v xcrun >/dev/null 2>&1 || { echo "xcrun not found; iOS build needs Xcode"; exit 1; }
	mkdir -p $(IOS_OBJDIR)/sim-x86_64
	@for src in $(MOBILE_SRCS); do \
	    obj=$(IOS_OBJDIR)/sim-x86_64/$$(echo $$src | sed 's|/|_|g; s|\.c$$|.o|'); \
	    xcrun --sdk iphonesimulator clang $(MOBILE_CFLAGS_BASE) $(IOS_DEFINES) \
	        -arch x86_64 -mios-simulator-version-min=$(IOS_MIN_VERSION) \
	        -isysroot $$(xcrun --sdk iphonesimulator --show-sdk-path) \
	        -c $$src -o $$obj || exit 1; \
	done
	xcrun libtool -static -o $(IOS_OBJDIR)/xs-ios-sim-x86_64.a $(IOS_OBJDIR)/sim-x86_64/*.o

# Apple Silicon simulator slice. Build separately and consume via
# xcframework when targeting arm64 macs.
ios-sim-arm64:
	@command -v xcrun >/dev/null 2>&1 || { echo "xcrun not found; iOS build needs Xcode"; exit 1; }
	mkdir -p $(IOS_OBJDIR)/sim-arm64
	@for src in $(MOBILE_SRCS); do \
	    obj=$(IOS_OBJDIR)/sim-arm64/$$(echo $$src | sed 's|/|_|g; s|\.c$$|.o|'); \
	    xcrun --sdk iphonesimulator clang $(MOBILE_CFLAGS_BASE) $(IOS_DEFINES) \
	        -arch arm64 -mios-simulator-version-min=$(IOS_MIN_VERSION) \
	        -isysroot $$(xcrun --sdk iphonesimulator --show-sdk-path) \
	        -c $$src -o $$obj || exit 1; \
	done
	xcrun libtool -static -o $(IOS_OBJDIR)/xs-ios-sim-arm64.a $(IOS_OBJDIR)/sim-arm64/*.o

# ---- Android --------------------------------------------------------
# Builds a per-ABI .so via the NDK and bundles them as a fat archive.
# Set ANDROID_NDK to the NDK root (r25+ recommended). API level 24 is
# the lowest that has full pthread + stdatomic support.
ANDROID_NDK ?= $(ANDROID_HOME)/ndk/25.2.9519653
ANDROID_API ?= 24
ANDROID_ABIS ?= arm64-v8a armeabi-v7a x86_64
ANDROID_OBJDIR = build/android
ANDROID_DEFINES = $(MOBILE_DEFINES) -DXS_ANDROID=1 -DXSC_ENABLE_JIT \
                  -DXSC_ENABLE_PLUGINS -DXSC_ENABLE_FMT \
                  -DXSC_ENABLE_TRANSPILER

# Map an Android ABI to the NDK's clang triple.
android_triple = $(strip \
    $(if $(filter arm64-v8a,$1),aarch64-linux-android, \
    $(if $(filter armeabi-v7a,$1),armv7a-linux-androideabi, \
    $(if $(filter x86_64,$1),x86_64-linux-android, \
    $(if $(filter x86,$1),i686-linux-android, \
    $(error unknown Android ABI $1))))))

android: android-clean
	@test -d $(ANDROID_NDK) || { echo "ANDROID_NDK not found at $(ANDROID_NDK)"; exit 1; }
	mkdir -p $(ANDROID_OBJDIR)
	for abi in $(ANDROID_ABIS); do \
	    triple=$$($(MAKE) -s android-print-triple ABI=$$abi); \
	    cc=$(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$${triple}$(ANDROID_API)-clang; \
	    test -x $$cc || cc=$(ANDROID_NDK)/toolchains/llvm/prebuilt/darwin-x86_64/bin/$${triple}$(ANDROID_API)-clang; \
	    mkdir -p $(ANDROID_OBJDIR)/$$abi; \
	    for src in $(MOBILE_SRCS) $(wildcard src/jit/*.c) \
	               $(wildcard src/plugins/*.c) \
	               $(wildcard src/transpiler/*.c) \
	               src/fmt/fmt.c; do \
	        obj=$(ANDROID_OBJDIR)/$$abi/$$(echo $$src | sed 's|/|_|g; s|\.c$$|.o|'); \
	        $$cc $(MOBILE_CFLAGS_BASE) $(ANDROID_DEFINES) -fPIC \
	            -c $$src -o $$obj || exit 1; \
	    done; \
	    $$cc -shared -Wl,--gc-sections -o $(ANDROID_OBJDIR)/$$abi/libxs.so \
	        $(ANDROID_OBJDIR)/$$abi/*.o -lm -llog || exit 1; \
	done
	@echo "built Android .so for: $(ANDROID_ABIS)"
	@for abi in $(ANDROID_ABIS); do \
	    echo "  $$abi: $$(wc -c < $(ANDROID_OBJDIR)/$$abi/libxs.so) bytes"; \
	done

android-print-triple:
	@echo $(call android_triple,$(ABI))

android-clean:
	rm -rf $(ANDROID_OBJDIR)

# ---- ESP32 / RISC-V microcontrollers --------------------------------
# Produces a static archive libxs.a configured for the ESP-IDF
# component layout. Drops everything that needs an MMU, an FS, or
# more than a megabyte of code: JIT, FFI, transpilers, HTTP server,
# the BearSSL TLS engine and X.509 verifier, doc / lint / pkg tools.
# Keeps the VM, the embed entrypoint, and `xs_run_bytecode` so an
# IDF app can flash a precompiled .xsc and execute it on boot.
ESP32_OBJDIR = build/esp32
ESP32_DROP = $(wildcard src/jit/*.c) \
             $(wildcard src/transpiler/*.c) \
             $(wildcard src/plugins/*.c) \
             src/runtime/builtins_ffi.c \
             src/runtime/builtins_http.c \
             src/runtime/builtins_db.c \
             src/runtime/builtins_crypto.c \
             src/runtime/builtins_process.c \
             src/runtime/builtins_async.c \
             src/runtime/builtins_thread.c \
             src/runtime/builtins_net.c \
             src/runtime/builtins_reactive.c \
             src/net/http_server.c \
             src/db/xsdb.c \
             src/doc/docgen.c src/lint/lint.c src/pkg/pkg.c \
             src/fmt/fmt.c \
             src/coverage/coverage.c
ESP32_SRCS = $(filter-out $(ESP32_DROP),$(MOBILE_SRCS))
ESP32_DEFINES = -DXS_ESP32=1 -DXS_NO_JIT=1 -DXS_NO_FS=1 \
                -DXS_NO_BEARSSL=1 \
                -DXSC_ENABLE_VM -DXSC_ENABLE_SANDBOX

# IDF_PATH must point at a checked-out esp-idf v5.x. The xtensa-esp32
# toolchain is normally on PATH after `. $$IDF_PATH/export.sh`.
ESP32_CC ?= xtensa-esp32-elf-gcc
ESP32_AR ?= xtensa-esp32-elf-ar
ESP32_CFLAGS = -Os -std=c11 -Isrc -Wall -Wno-unused-parameter \
               -ffunction-sections -fdata-sections \
               -fno-builtin -fno-stack-protector \
               $(ESP32_DEFINES)

esp32:
	@command -v $(ESP32_CC) >/dev/null 2>&1 || { \
	    echo "$(ESP32_CC) not on PATH; source esp-idf/export.sh first"; exit 1; }
	mkdir -p $(ESP32_OBJDIR)
	for src in $(ESP32_SRCS); do \
	    obj=$(ESP32_OBJDIR)/$$(echo $$src | sed 's|/|_|g; s|\.c$$|.o|'); \
	    $(ESP32_CC) $(ESP32_CFLAGS) -c $$src -o $$obj || exit 1; \
	done
	$(ESP32_AR) rcs $(ESP32_OBJDIR)/libxs.a $(ESP32_OBJDIR)/*.o
	@echo "built $(ESP32_OBJDIR)/libxs.a ($$(wc -c < $(ESP32_OBJDIR)/libxs.a) bytes)"
	@echo "drop into your esp-idf component as components/xs/libxs.a"

# Generate a minimal IDF component scaffold under examples/embedded/esp32/.
# After running `make esp32-component`, copy the directory into your
# project's `components/` and `idf.py build` will pick it up.
esp32-component:
	mkdir -p examples/embedded/esp32/components/xs/include
	cp src/xs_embed.h examples/embedded/esp32/components/xs/include/
	@printf 'idf_component_register(\n    SRCS "xs_stub.c"\n    INCLUDE_DIRS "include"\n    PRIV_REQUIRES esp_system)\n' \
	    > examples/embedded/esp32/components/xs/CMakeLists.txt
	@printf '/* placeholder; replace with libxs.a built via `make esp32` */\n' \
	    > examples/embedded/esp32/components/xs/xs_stub.c
	@echo "scaffold written to examples/embedded/esp32/components/xs/"

clean:
	rm -f $(OBJS) $(TARGET)
	find src -name '*.o' -delete
	rm -rf build/ios build/android build/esp32
