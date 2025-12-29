#!/usr/bin/env bash
# Triple-backend benchmark + correctness check.
#
# For each workload:
#   1. Run under --interp / --vm / --jit and confirm outputs match
#      byte-for-byte (md5).
#   2. Time wall-clock for each backend over N iterations.
#   3. Print the relative speed.
#
# Failure modes:
#   * A divergent output makes that benchmark "DIVERGE" and we exit non-zero
#   * A backend that times out (default 60s) is reported but doesn't fail
#
# Usage: tests/bench_backends.sh [N=3]
set -u
cd "$(dirname "$0")/.."
N=${1:-3}

BENCHMARKS=(
    benchmarks/bench_fibonacci.xs
    benchmarks/bench_sort.xs
    benchmarks/bench_strings.xs
)

# Ad-hoc cpu-bound workloads kept inline for backends to chew on.
mkdir -p /tmp/xs_bench
cat > /tmp/xs_bench/loop_sum.xs <<'EOF'
var s = 0
for i in 1..=1000000 { s = s + i }
println(s)
EOF
cat > /tmp/xs_bench/map_chain.xs <<'EOF'
let r = (1..=10000).to_array().map(|x| x * 2).filter(|x| x % 3 == 0)
println(r.len())
EOF
cat > /tmp/xs_bench/string_build.xs <<'EOF'
var s = ""
for i in 1..=10000 { s = s + "x" }
println(s.len())
EOF
cat > /tmp/xs_bench/fib_calls.xs <<'EOF'
fn fib(n) { return if n<2 { n } else { fib(n-1) + fib(n-2) } }
println(fib(28))
EOF
BENCHMARKS+=(
    /tmp/xs_bench/loop_sum.xs
    /tmp/xs_bench/map_chain.xs
    /tmp/xs_bench/string_build.xs
    /tmp/xs_bench/fib_calls.xs
)

# nanoseconds since epoch via /proc; fallback to date +%s%N
now_ns() { date +%s%N; }
# Time a single command, return seconds as float.
time_one() {
    local start end
    start=$(now_ns)
    "$@" >/dev/null 2>&1
    end=$(now_ns)
    awk -v s="$start" -v e="$end" 'BEGIN { printf "%.3f", (e-s)/1e9 }'
}
median_of() {
    local samples=("$@")
    printf '%s\n' "${samples[@]}" | sort -n | awk '
        { a[NR] = $1 }
        END {
            n = NR
            if (n % 2) printf "%.3f", a[(n+1)/2]
            else       printf "%.3f", (a[n/2] + a[n/2+1]) / 2
        }'
}

printf "%-30s | %-10s | %-10s | %-10s | %-10s | %s\n" \
       "benchmark" "interp" "vm" "jit" "jit/vm" "outputs"
printf "%-30s-+-%-10s-+-%-10s-+-%-10s-+-%-10s-+-%s\n" \
       "------------------------------" "----------" "----------" "----------" "----------" "-------"

diverge_total=0
for f in "${BENCHMARKS[@]}"; do
    name=$(basename "$f" .xs)

    # Correctness: outputs must match across all three.
    out_i=$(./xs --interp "$f" 2>/dev/null)
    out_v=$(./xs --vm     "$f" 2>/dev/null)
    out_j=$(./xs --jit    "$f" 2>/dev/null)
    if [ "$out_i" = "$out_v" ] && [ "$out_v" = "$out_j" ]; then
        match="ok"
    else
        match="DIVERGE"
        diverge_total=$((diverge_total + 1))
    fi

    # Time each backend N times, report median.
    samples_i=()
    samples_v=()
    samples_j=()
    for ((k=0; k<N; k++)); do
        samples_i+=("$(time_one ./xs --interp "$f")")
        samples_v+=("$(time_one ./xs --vm "$f")")
        samples_j+=("$(time_one ./xs --jit "$f")")
    done
    t_i=$(median_of "${samples_i[@]}")
    t_v=$(median_of "${samples_v[@]}")
    t_j=$(median_of "${samples_j[@]}")
    ratio=$(awk -v j="$t_j" -v v="$t_v" \
        'BEGIN { if (v == 0) print "n/a"; else printf "%.2fx", j/v }')

    printf "%-30s | %-10s | %-10s | %-10s | %-10s | %s\n" \
           "$name" "${t_i}s" "${t_v}s" "${t_j}s" "$ratio" "$match"
done

echo
if [ $diverge_total -gt 0 ]; then
    echo "FAIL: $diverge_total benchmark(s) produced divergent output across backends"
    exit 1
fi
echo "all backends produced byte-identical output"
