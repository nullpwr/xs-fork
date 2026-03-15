#!/usr/bin/env bash
# Cross-runtime benchmark harness.
# Times each bench under xs (interp, vm, jit), python3, node, go.
# Emits a markdown table + results.json.
set -u
cd "$(dirname "$0")"

# Repeat count: single-run timings are noisy; we take the best-of-N.
: "${REPS:=5}"
: "${WARMUP:=1}"

XS="../xs"

# List of (name, xs-source, py-source, js-source, go-source)
BENCHES=(
    "fib              bench_fib.xs              bench_fib.py              bench_fib.js              bench_fib.go"
    "sort             bench_sort.xs             bench_sort.py             bench_sort.js             bench_sort.go"
    "mandelbrot       bench_mandelbrot.xs       bench_mandelbrot.py       bench_mandelbrot.js       bench_mandelbrot.go"
    "nbody            bench_nbody.xs            bench_nbody.py            bench_nbody.js            bench_nbody.go"
    "json_parse       bench_json.xs             bench_json.py             bench_json.js             bench_json.go"
    "string_munge     bench_strings.xs          bench_strings.py          bench_strings.js          bench_strings.go"
    "hash             bench_hash.xs             bench_hash.py             bench_hash.js             bench_hash.go"
    "startup          bench_startup.xs          bench_startup.py          bench_startup.js          bench_startup.go"
)

# Best-of-N wall time in seconds (as float).
time_cmd() {
    local best="" t
    for _ in $(seq 1 "$WARMUP"); do
        "$@" >/dev/null 2>&1 || return 1
    done
    for _ in $(seq 1 "$REPS"); do
        t=$({ TIMEFORMAT='%R'; time "$@" >/dev/null 2>&1; } 2>&1)
        if [ -z "$best" ] || awk "BEGIN {exit !($t < $best)}"; then
            best=$t
        fi
    done
    echo "$best"
}

say_row() {
    printf "| %-14s | %10s | %10s | %10s | %10s | %10s | %10s |\n" "$@"
}

say_sep() {
    printf "|%s|%s|%s|%s|%s|%s|%s|\n" \
        "----------------" "------------" "------------" "------------" \
        "------------" "------------" "------------"
}

echo ""
say_row "bench" "xs --interp" "xs --vm" "xs --jit" "python3" "node" "go"
say_sep

declare -A RESULTS
for line in "${BENCHES[@]}"; do
    # shellcheck disable=SC2206
    arr=($line)
    name=${arr[0]}; xs=${arr[1]}; py=${arr[2]}; js=${arr[3]}; go=${arr[4]}

    if [ ! -f "$xs" ]; then continue; fi

    t_interp=$(time_cmd "$XS" --interp "$xs" || echo "-")
    t_vm=$(time_cmd     "$XS" --vm     "$xs" || echo "-")
    t_jit=$(time_cmd    "$XS" --jit    "$xs" || echo "-")
    t_py=$(test -f "$py" && time_cmd python3 "$py" || echo "-")
    t_node=$(test -f "$js" && time_cmd node "$js" || echo "-")
    if [ -f "$go" ]; then
        go build -o /tmp/xs-bench-go "$go" >/dev/null 2>&1 && t_go=$(time_cmd /tmp/xs-bench-go) || t_go="-"
        rm -f /tmp/xs-bench-go
    else
        t_go="-"
    fi

    RESULTS["$name.interp"]=$t_interp
    RESULTS["$name.vm"]=$t_vm
    RESULTS["$name.jit"]=$t_jit
    RESULTS["$name.py"]=$t_py
    RESULTS["$name.node"]=$t_node
    RESULTS["$name.go"]=$t_go

    say_row "$name" "$t_interp" "$t_vm" "$t_jit" "$t_py" "$t_node" "$t_go"
done

echo ""

# Emit JSON for CI comparison
{
    printf '{\n'
    printf '  "repo_commit": "%s",\n' "$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    printf '  "reps": %s,\n' "$REPS"
    printf '  "timestamp": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '  "results": {\n'
    first=1
    for line in "${BENCHES[@]}"; do
        arr=($line); name=${arr[0]}
        [ -z "${RESULTS[$name.interp]:-}" ] && continue
        [ $first -eq 1 ] && first=0 || printf ',\n'
        printf '    "%s": { "interp": "%s", "vm": "%s", "jit": "%s", "python3": "%s", "node": "%s", "go": "%s" }' \
            "$name" "${RESULTS[$name.interp]}" "${RESULTS[$name.vm]}" "${RESULTS[$name.jit]}" \
            "${RESULTS[$name.py]}" "${RESULTS[$name.node]}" "${RESULTS[$name.go]}"
    done
    printf '\n  }\n'
    printf '}\n'
} > results.json

echo "wrote results.json"
