#!/bin/bash
# Cross-backend matrix over the regression corpus. Every .xs file under
# tests/regression is run through the interpreter (baseline), then
# through every other backend (vm, jit, --emit c at -O0 and -O2,
# --emit js via node, --emit wasm via wasmtime). Stdout and exit code
# must match the baseline for every backend that the file isn't opted
# out of.
#
# Skip markers (first 5 lines of the .xs file):
#
#   -- skip-emit: c, js, wasm
#       any subset of those three; that backend is silently skipped.
#       Use when the test exercises a feature the transpiler can't
#       (or won't) lower, with a TODO note for what's missing.
#
#   -- skip-backend: jit
#       (already honoured by run-all.sh layer R)
#       also honoured here for the jit leg.
#
# Files that have no skip markers must match across every backend
# that's available in the build environment.
#
# Golden and property suites are out of scope: golden snapshots compare
# an AST or formatted-error dump that only the interp / front-end emits,
# and property tests load a harness via `load "tests/property/..."`
# which only resolves when run via tests/property/run.sh (which cd's
# into the project root first). Layers 7 and 4 cover them already.

set -u
cd "$(dirname "$0")/.."

if [ -f tests/lsan.supp ]; then
    export LSAN_OPTIONS="${LSAN_OPTIONS:-exitcode=0:suppressions=$PWD/tests/lsan.supp:print_suppressions=0}"
fi

# Tooling availability
have_node=0
have_wasmtime=0
have_gcc=0
command -v gcc      > /dev/null 2>&1 && have_gcc=1
command -v node     > /dev/null 2>&1 && have_node=1
command -v wasmtime > /dev/null 2>&1 && have_wasmtime=1

pass=0
fail=0
skipped_files=0
declare -a failures

# Strip the trailing one-line exit reason that XS_DEBUG_EXIT may append
# to stderr when the binary exits non-zero. Our baseline runs without
# it; suppress it in the children too for consistency.
unset XS_DEBUG_EXIT

# Files whose stdout legitimately varies between runs (timestamps,
# tempfile paths, network responses, sampling profilers). Diffing
# their output across backends is meaningless; skip wholesale.
SKIP_FILES=(
    # placeholder; per-file skip-emit markers cover the rest.
)

is_in_skip_files() {
    local target="$1" sf
    if [ "${#SKIP_FILES[@]}" -eq 0 ]; then return 1; fi
    for sf in "${SKIP_FILES[@]}"; do [ "$target" = "$sf" ] && return 0; done
    return 1
}

# Read the first 5 lines and look for a `-- skip-emit: ...` line. The
# list after the colon is comma- or space-separated. Set the per-call
# variables skip_c / skip_js / skip_wasm / skip_jit accordingly.
read_skip_markers() {
    local f="$1"
    skip_c=0; skip_js=0; skip_wasm=0; skip_jit=0
    local hdr line list
    hdr=$(head -5 "$f" 2>/dev/null)
    while IFS= read -r line; do
        case "$line" in
            *"skip-emit:"*)
                list=${line#*skip-emit:}
                # normalise: lowercase, strip commas, strip parens/comments
                list=$(echo "$list" | tr ',' ' ' | tr -d '()' | tr '[:upper:]' '[:lower:]')
                for tok in $list; do
                    case "$tok" in
                        c)    skip_c=1 ;;
                        js)   skip_js=1 ;;
                        wasm) skip_wasm=1 ;;
                        all)  skip_c=1; skip_js=1; skip_wasm=1 ;;
                    esac
                done
                ;;
            *"skip-backend:"*)
                list=${line#*skip-backend:}
                list=$(echo "$list" | tr ',' ' ' | tr -d '()' | tr '[:upper:]' '[:lower:]')
                for tok in $list; do
                    case "$tok" in
                        jit) skip_jit=1 ;;
                    esac
                done
                ;;
        esac
    done <<<"$hdr"
}

# Normalise the stdout of an xs run so that environment-specific bits
# (host paths, addresses) do not show as divergence. Mirrors the scrub
# in tests/run.sh.
scrub() {
    sed -E \
        -e 's|/tmp/[A-Za-z0-9._/-]+|<TMP>|g' \
        -e 's/[0-9]+\.[0-9]+e[+-][0-9]+/<float>/g' \
        -e 's/0x[0-9a-fA-F]+/<ptr>/g'
}

run_file() {
    local f="$1"
    local name=${f#tests/}
    local tmp
    tmp=$(mktemp -d)

    # Stdout only is what counts as "program output." Lint warnings,
    # debug spam, and the XS_DEBUG_EXIT one-liner all land on stderr;
    # diffing them as part of the matrix would just flag noise between
    # backends that do or don't bother re-running the linter.
    local interp_out interp_rc
    interp_out=$(./xs --interp "$f" 2>"$tmp/interp_err.txt"); interp_rc=$?
    local base
    base=$(echo "$interp_out" | scrub)

    read_skip_markers "$f"

    local file_failures=""
    local backends_run=0

    compare() {
        local label="$1" got="$2" got_rc="$3"
        local g
        g=$(echo "$got" | scrub)
        if [ "$g" != "$base" ] || [ "$got_rc" != "$interp_rc" ]; then
            file_failures="$file_failures
  $label rc=$got_rc (baseline rc=$interp_rc)
$(diff <(echo "$base") <(echo "$g") | head -12 | sed 's/^/    /')"
            return 1
        fi
        return 0
    }

    # VM is always required.
    local vm_out vm_rc
    vm_out=$(./xs --vm "$f" 2>"$tmp/vm_err.txt"); vm_rc=$?
    compare "vm" "$vm_out" "$vm_rc"
    backends_run=$((backends_run + 1))

    # JIT (honour both the legacy skip-backend: jit marker and a fall-back
    # message printed when the JIT can't emit code on this host).
    if [ $skip_jit -eq 0 ]; then
        local jit_out jit_rc jit_err
        jit_out=$(./xs --jit "$f" 2>"$tmp/jit_err.txt"); jit_rc=$?
        jit_err=$(cat "$tmp/jit_err.txt" 2>/dev/null)
        if echo "$jit_err" | grep -q "falling back to VM"; then
            :  # transparent fallback, not a separate backend
        else
            compare "jit" "$jit_out" "$jit_rc"
            backends_run=$((backends_run + 1))
        fi
    fi

    # C backend: transpile, compile at -O0 and -O2, run. Only when
    # gcc is available (always on the platforms we test).
    if [ $skip_c -eq 0 ] && [ $have_gcc -eq 1 ]; then
        if ./xs --emit c "$f" > "$tmp/prog.c" 2>"$tmp/emit_c_err.txt"; then
            if gcc -o "$tmp/prog_c0" "$tmp/prog.c" -lm -lpthread 2>"$tmp/gcc0.txt"; then
                local c0_out c0_rc
                c0_out=$("$tmp/prog_c0" 2>"$tmp/c0_err.txt"); c0_rc=$?
                compare "c -O0" "$c0_out" "$c0_rc"
                backends_run=$((backends_run + 1))
                if gcc -O2 -o "$tmp/prog_c2" "$tmp/prog.c" -lm -lpthread 2>"$tmp/gcc2.txt"; then
                    local c2_out c2_rc
                    c2_out=$("$tmp/prog_c2" 2>"$tmp/c2_err.txt"); c2_rc=$?
                    compare "c -O2" "$c2_out" "$c2_rc"
                    backends_run=$((backends_run + 1))
                else
                    file_failures="$file_failures
  c -O2 compile failed:
$(head -6 "$tmp/gcc2.txt" | sed 's/^/    /')"
                fi
            else
                file_failures="$file_failures
  c -O0 compile failed:
$(head -6 "$tmp/gcc0.txt" | sed 's/^/    /')"
            fi
        else
            file_failures="$file_failures
  --emit c transpile failed:
$(head -6 "$tmp/emit_c_err.txt" | sed 's/^/    /')"
        fi
    fi

    # JS backend
    if [ $skip_js -eq 0 ] && [ $have_node -eq 1 ]; then
        if ./xs --emit js "$f" > "$tmp/prog.js" 2>"$tmp/emit_js_err.txt"; then
            local js_out js_rc
            js_out=$(node "$tmp/prog.js" 2>"$tmp/js_err.txt"); js_rc=$?
            compare "js" "$js_out" "$js_rc"
            backends_run=$((backends_run + 1))
        else
            file_failures="$file_failures
  --emit js transpile failed:
$(head -6 "$tmp/emit_js_err.txt" | sed 's/^/    /')"
        fi
    fi

    # WASM backend
    if [ $skip_wasm -eq 0 ] && [ $have_wasmtime -eq 1 ]; then
        if ./xs --emit wasm "$f" > "$tmp/prog.wasm" 2>"$tmp/emit_wasm_err.txt"; then
            local wa_out wa_rc
            wa_out=$(wasmtime "$tmp/prog.wasm" 2>"$tmp/wasm_err.txt"); wa_rc=$?
            compare "wasm" "$wa_out" "$wa_rc"
            backends_run=$((backends_run + 1))
        else
            file_failures="$file_failures
  --emit wasm transpile failed:
$(head -6 "$tmp/emit_wasm_err.txt" | sed 's/^/    /')"
        fi
    fi

    if [ -z "$file_failures" ]; then
        pass=$((pass + 1))
        if [ "${CORPUS_VERBOSE:-0}" = "1" ]; then
            echo "  ok    $name ($backends_run backends)"
        fi
    else
        fail=$((fail + 1))
        failures+=("$name$file_failures")
        echo "  FAIL  $name"
        echo "$file_failures" | head -20 | sed 's/^/      /'
    fi

    rm -rf "$tmp"
}

# Collect every .xs file under the regression corpus. Golden and
# property suites are deliberately out of scope here:
#
# - tests/golden/{ast,errors} compare a fixed snapshot of `--emit ast`
#   or compile-time error formatting against a .expected file. They
#   have nothing to do with runtime behaviour across backends; Layer 7
#   already validates them.
#
# - tests/property/*.xs use a `load "tests/property/harness.xs"` line
#   resolved from the project root via tests/property/run.sh (`cd`s
#   into the project root before invoking xs). Running them as
#   standalone files breaks that load path. Layer 4 already validates
#   them, and the harness itself only generates inputs for runtime
#   semantics tests, which already get cross-backend coverage via
#   conformance / regression.
declare -a CORPUS_FILES
for dir in tests/regression; do
    [ -d "$dir" ] || continue
    for f in "$dir"/*.xs; do
        [ -f "$f" ] || continue
        if is_in_skip_files "$f"; then
            skipped_files=$((skipped_files + 1))
            continue
        fi
        CORPUS_FILES+=("$f")
    done
done

echo "corpus matrix: ${#CORPUS_FILES[@]} files (node=$have_node, wasmtime=$have_wasmtime)"
for f in "${CORPUS_FILES[@]}"; do
    run_file "$f"
done

echo ""
echo "corpus matrix: $pass passed, $fail failed (${skipped_files} library files skipped)"
[ $fail -eq 0 ] && exit 0 || exit 1
