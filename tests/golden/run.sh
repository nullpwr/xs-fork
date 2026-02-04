#!/usr/bin/env bash
# Layer 7: golden/snapshot tests.
#
#   tests/golden/ast/foo.xs      -> compared against foo.expected, using
#                                   xs --emit ast --no-color
#   tests/golden/errors/bar.xs   -> compared against bar.expected, using
#                                   xs --no-color (expects compile to fail)
#
# Paths in actual output are normalised to <FILE> before diffing so the
# fixtures are portable.
#
# Regenerate a broken snapshot with: UPDATE_GOLDEN=1 tests/golden/run.sh
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/.." && pwd)"
XS="${XS:-$ROOT/../xs}"
[ -x "$XS" ] || XS="$ROOT/../xs.exe"
if [ ! -x "$XS" ]; then echo "no xs binary at $XS" >&2; exit 2; fi

normalize() {
    # Replace the full file path (and its relative forms) with <FILE>.
    # On Windows the binary prints paths like D:/a/xs/xs/tests/...
    # so matching the sandbox-specific $src isn't enough. The second
    # sed swap grabs any non-space run ending in the test basename,
    # which covers POSIX, Windows drive, and relative variants.
    local src="$1"
    local abs rel base
    abs="$src"
    rel="${src#$ROOT/../}"
    base="$(basename "$src")"
    sed -E \
        -e "s|$abs|<FILE>|g" \
        -e "s|$rel|<FILE>|g" \
        -e "s#[^ [:space:]\"'\`]*$base#<FILE>#g"
}

run_group() {
    local group="$1" emit="$2"
    local gdir="$DIR/$group"
    [ -d "$gdir" ] || return 0

    local pass=0 fail=0
    for f in "$gdir"/*.xs; do
        [ -f "$f" ] || continue
        local name exp actual
        name="$(basename "$f" .xs)"
        exp="$gdir/$name.expected"

        if [ "$emit" = "ast" ]; then
            actual="$("$XS" --emit ast --no-color "$f" 2>&1)"
        else
            actual="$("$XS" --no-color "$f" 2>&1 || true)"
        fi
        # Strip CRs so golden fixtures compare cleanly on Windows.
        # Both sides need scrubbing: msys stdout may come back CRLF,
        # and git's autocrlf on Windows may check out .expected files
        # with CRLF endings.
        actual="${actual//$'\r'/}"
        actual="$(echo "$actual" | normalize "$f")"

        if [ ! -f "$exp" ]; then
            if [ "${UPDATE_GOLDEN:-0}" = "1" ]; then
                echo "$actual" > "$exp"
                echo "  NEW   $group/$name (golden written)"
                pass=$((pass + 1))
            else
                echo "  FAIL  $group/$name (no .expected, run UPDATE_GOLDEN=1)"
                fail=$((fail + 1))
            fi
            continue
        fi

        local expected
        expected="$(cat "$exp")"
        expected="${expected//$'\r'/}"

        if [ "$actual" = "$expected" ]; then
            pass=$((pass + 1))
        else
            if [ "${UPDATE_GOLDEN:-0}" = "1" ]; then
                echo "$actual" > "$exp"
                echo "  UPD   $group/$name (golden updated)"
                pass=$((pass + 1))
            else
                fail=$((fail + 1))
                echo "  FAIL  $group/$name"
                # diff isn't a hard dep; fall back to raw dump when the
                # environment (e.g. bare msys2) doesn't ship diffutils.
                if command -v diff > /dev/null 2>&1; then
                    diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") | head -12 | sed 's/^/        /'
                else
                    echo "        --- expected ---"
                    printf '%s\n' "$expected" | head -6 | sed 's/^/        /'
                    echo "        --- actual ---"
                    printf '%s\n' "$actual" | head -6 | sed 's/^/        /'
                fi
            fi
        fi
    done

    echo "  [$group] $pass passed, $fail failed"
    return $fail
}

total_fail=0
run_group ast ast    || total_fail=$((total_fail + $?))
run_group errors run || total_fail=$((total_fail + $?))

if [ $total_fail -gt 0 ]; then
    echo "[golden] $total_fail snapshot mismatches"
    exit 1
fi
echo "[golden] all snapshots match"
