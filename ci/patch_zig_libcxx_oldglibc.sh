#!/bin/bash
# Patch a zig install's bundled libc++ so C++17 aligned new/delete links for
# old glibc (<2.16) targets — no zig rebuild, no compat object, no -D flags.
#
# Usage: patch_zig_libcxx_oldglibc.sh <zig-install-dir>
#        patch_zig_libcxx_oldglibc.sh --zig-prefix <zig-install-dir>
#
# Applies pre-generated diffs from scripts/zig-patches/ with `patch -N`
# (idempotent: re-running is a no-op). Then self-verifies and, on failure,
# restores from .anyfsbak backups.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_DIR="$SCRIPT_DIR/zig-patches"

die() { echo "patch_zig_libcxx_oldglibc: $*" >&2; exit 1; }

# ---- arg parsing ----
case "${1:-}" in
    --zig-prefix) ZIG_PREFIX="${2:?--zig-prefix needs a dir}" ;;
    "")           die "usage: $0 <zig-install-dir>" ;;
    *)            ZIG_PREFIX="$1" ;;
esac
[ -d "$ZIG_PREFIX" ] || die "not a directory: $ZIG_PREFIX"

ZIG_BIN="$ZIG_PREFIX/zig"
[ -x "$ZIG_BIN" ] || die "no zig binary at $ZIG_BIN"

LIBCXX="$ZIG_PREFIX/lib/libcxx"
[ -f "$LIBCXX/include/__config" ] || die "no libc++ __config under $LIBCXX"

# ---- era detection ----
if [ -f "$LIBCXX/src/include/aligned_alloc.h" ]; then
    ERA="0.17-dev"
    PATCHES=(config-0.17-dev.patch aligned_alloc-0.17-dev.patch)
else
    ERA="0.16.0"
    PATCHES=(config-0.16.0.patch)
fi
echo "patch_zig_libcxx_oldglibc: era=$ERA prefix=$ZIG_PREFIX"

# Restore originals from our deterministic .anyfsbak backups, then remove any .rej.
restore_backups() {
    find "$LIBCXX" -name '*.anyfsbak' -print0 | while IFS= read -r -d '' b; do
        mv -f "$b" "${b%.anyfsbak}"
    done
    find "$LIBCXX" -name '*.rej' -delete
}

# ---- apply ----
# _applied_any tracks whether any patch was NEWLY applied this run. If every patch was already
# applied (idempotent re-run, e.g. the prep image rebuilt on the same zig), the tree was
# already verified on the run that applied it, so the slow self-verify (which recompiles libc++
# twice) is skipped.
_applied_any=0
apply_one() {
    local pf="$PATCH_DIR/$1" out rc
    [ -f "$pf" ] || die "missing patch file: $pf"
    out="$(patch -N -b --suffix=.anyfsbak -p1 --fuzz=3 -d "$LIBCXX" < "$pf" 2>&1)" && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "patch_zig_libcxx_oldglibc: applied $1"
        _applied_any=1
    elif printf '%s\n' "$out" | grep -qiE 'previously applied|Reversed'; then
        echo "patch_zig_libcxx_oldglibc: $1 already applied (skipping)"
    else
        printf '%s\n' "$out" >&2
        restore_backups
        die "failed to apply $1; libc++ restored from backups (see error above)"
    fi
}
for p in "${PATCHES[@]}"; do apply_one "$p"; done

# Nothing newly applied → already-patched-and-verified zig; skip the expensive self-verify.
if [ "$_applied_any" -eq 0 ]; then
    echo "patch_zig_libcxx_oldglibc: all patches already applied — skipping self-verify"
    find "$LIBCXX" \( -name '*.anyfsbak' -o -name '*.rej' \) -delete 2>/dev/null || true
    exit 0
fi

# ---- self-verify (backstop against a fuzzed mis-apply) ----
PROBE_DIR="$(mktemp -d "${TMPDIR:-$(dirname "$ZIG_PREFIX")}/zigpatch.XXXXXX")"
trap 'rm -rf "$PROBE_DIR"' EXIT
cat > "$PROBE_DIR/anew.cpp" <<'EOF'
#include <new>
#include <cstdint>
struct alignas(64) Big { char x[64]; };
int main() { Big* p = new Big(); int r = (int)((uintptr_t)p & 63); delete p; return r; }
EOF
export ZIG_GLOBAL_CACHE_DIR="$PROBE_DIR/zc"   # isolated cache -> real libc++ recompile

verify_target() {  # <triple> <want-symbol> <forbid-symbol>
    local triple="$1" want="$2" forbid="$3" out="$PROBE_DIR/probe.bin" syms
    if ! "$ZIG_BIN" c++ -target "$triple" -std=c++17 "$PROBE_DIR/anew.cpp" -o "$out" 2>"$PROBE_DIR/err"; then
        echo "  verify FAIL: $triple did not link" >&2
        grep -oE 'undefined symbol:[^>]*' "$PROBE_DIR/err" | sort -u >&2
        return 1
    fi
    syms="$(objdump -T "$out" 2>/dev/null | grep -oE 'aligned_alloc|posix_memalign' | sort -u | tr '\n' ',')"
    if [ -n "$want" ]   && ! printf '%s' "$syms" | grep -q "$want";   then echo "  verify FAIL: $triple missing $want (got [$syms])" >&2; return 1; fi
    if [ -n "$forbid" ] &&   printf '%s' "$syms" | grep -q "$forbid"; then echo "  verify FAIL: $triple uses forbidden $forbid (got [$syms])" >&2; return 1; fi
    echo "  verify OK: $triple -> [$syms]"
}

VERIFY_OK=1
verify_target x86_64-linux-gnu.2.11 posix_memalign aligned_alloc || VERIFY_OK=0
verify_target x86_64-linux-gnu.2.17 aligned_alloc ""             || VERIFY_OK=0

# On a clean first apply this rolls back via the .anyfsbak backups. On an
# idempotent re-run patch makes no backup, so restore is a no-op — but the tree
# is already correctly patched, so there is nothing to roll back to anyway.
if [ "$VERIFY_OK" -ne 1 ]; then
    restore_backups
    die "self-verify failed; libc++ restored from backups"
fi

# success: drop our backups and any stray .rej (e.g. from an idempotent re-run)
find "$LIBCXX" \( -name '*.anyfsbak' -o -name '*.rej' \) -delete
echo "patch_zig_libcxx_oldglibc: OK (era=$ERA)"
