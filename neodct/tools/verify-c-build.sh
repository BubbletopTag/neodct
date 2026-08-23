#!/bin/sh
#
# The acceptance gate for the C port. Run from the repository root.
#
# Agents report their own success. This does not take their word for it: it
# rebuilds from scratch, runs the tests itself, and diffs the rendered frames
# against the reference captured from the Python build. Every check prints
# PASS or FAIL and the script's exit status is the number of failures, so it
# can be wired into CI unchanged.
#
# The checks are ordered so the cheapest and most fundamental fail first --
# there is no point diffing pixels from a binary that only compiled because
# somebody removed -Werror.
#
#   ./neodct/tools/verify-c-build.sh            everything
#   ./neodct/tools/verify-c-build.sh build      just the compile checks
#   ./neodct/tools/verify-c-build.sh frames     just the golden comparison

set -u

SRC=neodct/src
GOLDEN=neodct/tests/golden
OUT=${OUT:-/tmp/nd-verify}
WHICH=${1:-all}

FAILURES=0
pass() { printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
fail() { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAILURES=$((FAILURES + 1)); }
skip() { printf '  \033[33mSKIP\033[0m  %s\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

want() { [ "$WHICH" = all ] || [ "$WHICH" = "$1" ]; }

rm -rf "$OUT"
mkdir -p "$OUT"

# --------------------------------------------------------------- build

if want build; then
head2 "1. Compiles clean with -Werror"
if [ ! -f "$SRC/Makefile" ]; then
    fail "$SRC/Makefile does not exist"
else
    if make -C "$SRC" clean >/dev/null 2>&1; then :; fi
    if make -C "$SRC" -j"$(nproc)" > "$OUT/build.log" 2>&1; then
        pass "make"
    else
        fail "make -- see $OUT/build.log"
        tail -25 "$OUT/build.log" | sed 's/^/        /'
    fi

    # A build that passes only because the warning flags were quietly
    # loosened is the failure mode this catches.
    head2 "2. The required warning flags are actually in use"
    for flag in -Werror -Wall -Wextra -Wshadow -Wconversion -Wstrict-prototypes -Wvla; do
        if grep -q -- "$flag" "$SRC/Makefile"; then
            pass "$flag present in Makefile"
        else
            fail "$flag MISSING from Makefile"
        fi
    done
fi

head2 "3. Compiles under musl (the target's libc)"
# COMPILE, not link. musl-gcc supplies libc only; freetype, libpng, libjpeg
# and sqlite on this host are glibc builds, so a musl *link* can never
# succeed here and failing on it would be noise. Compiling is what catches
# the thing we actually care about -- glibc-only functions and headers -- at
# the moment they are written rather than at the first cross-build.
# Real musl linking is verified by the Buildroot cross-build. See MUSL.md.
if ! command -v musl-gcc >/dev/null 2>&1; then
    skip "musl-gcc not installed"
else
    MUSL_FAILED=0
    : > "$OUT/musl.log"
    # jconfig.h and asm/types.h live in the multiarch directory, not
    # /usr/include proper. Same -idirafter reasoning as below.
    MULTIARCH=$(gcc -print-multiarch 2>/dev/null)
    MA_INC=""
    [ -n "$MULTIARCH" ] && [ -d "/usr/include/$MULTIARCH" ] && \
        MA_INC="-idirafter /usr/include/$MULTIARCH"
    for c in $(find "$SRC/lib" "$SRC/core" "$SRC/apprun" "$SRC/apps" "$SRC/tools" \
                    -name '*.c' 2>/dev/null); do
        # -idirafter, NOT -I: musl's own headers must win every lookup, so a
        # glibc-only libc call still fails here -- that is the entire point of
        # the check. /usr/include is searched only as a last resort, for
        # third-party headers musl does not ship (jpeglib.h, sqlite3.h) and
        # kernel UAPI headers (linux/fb.h, linux/input.h).
        if ! musl-gcc -std=c11 -c "$c" -o /dev/null -I"$SRC/include" \
             $(pkg-config --cflags freetype2 libpng sqlite3 libjpeg 2>/dev/null) \
             -idirafter /usr/include $MA_INC \
             -D_GNU_SOURCE -Wall -Wextra >> "$OUT/musl.log" 2>&1; then
            MUSL_FAILED=$((MUSL_FAILED + 1))
            echo "  ^^ in $c" >> "$OUT/musl.log"
        fi
    done
    if [ "$MUSL_FAILED" -eq 0 ]; then
        pass "all sources compile under musl-gcc"
    else
        fail "$MUSL_FAILED source(s) fail under musl -- see $OUT/musl.log"
        grep -B2 "\^\^ in" "$OUT/musl.log" | head -30 | sed 's/^/        /'
    fi
fi
fi

# --------------------------------------------------------------- tests

if want all || want tests; then
head2 "4. Unit tests"
if make -C "$SRC" test > "$OUT/test.log" 2>&1; then
    pass "make test"
    grep -iE "^[0-9]+ (tests?|assertions?)|passed|ok " "$OUT/test.log" | tail -5 | sed 's/^/        /'
else
    fail "make test -- see $OUT/test.log"
    tail -25 "$OUT/test.log" | sed 's/^/        /'
fi

head2 "5. Unit tests under AddressSanitizer + UBSan"
# The single most valuable check in this file. C's worst failures -- use
# after free, buffer overflow, uninitialised reads -- pass ordinary tests
# and surface weeks later on the device. ASan turns them into a stack trace
# at the moment they happen.
if make -C "$SRC" clean >/dev/null 2>&1 && \
   make -C "$SRC" ASAN=1 test > "$OUT/asan.log" 2>&1; then
    pass "make ASAN=1 test"
else
    fail "make ASAN=1 test -- see $OUT/asan.log"
    grep -A12 -iE "ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:" "$OUT/asan.log" \
        | head -40 | sed 's/^/        /'
fi
make -C "$SRC" clean >/dev/null 2>&1
make -C "$SRC" -j"$(nproc)" >/dev/null 2>&1
fi

# --------------------------------------------------------------- frames

if want all || want frames; then
head2 "6. Rendered frames match the Python reference"
SHOOT="$SRC/build/default/bin/nd-shoot"
if [ ! -x "$SHOOT" ]; then
    skip "nd-shoot not built yet"
else
    if "$SHOOT" --out "$OUT/frames" > "$OUT/shoot.log" 2>&1; then
        pass "nd-shoot ran"
        if python3 neodct/tools/goldenframe.py --compare "$GOLDEN" "$OUT/frames" \
               > "$OUT/compare.log" 2>&1; then
            pass "all frames identical to the Python build"
        else
            # Not automatically a failure of the whole gate: report the
            # detail, because "3 frames differ by 4 px" and "46 frames differ"
            # are very different situations and the numbers say which.
            fail "frames differ"
            sed 's/^/        /' "$OUT/compare.log" | head -30
        fi
    else
        fail "nd-shoot failed -- see $OUT/shoot.log"
        tail -15 "$OUT/shoot.log" | sed 's/^/        /'
    fi
fi

head2 "7. Glyph rendering matches Pillow"
GLYPH="$SRC/build/default/test/test_font"
if [ ! -x "$GLYPH" ]; then
    skip "test_font not built yet"
elif "$GLYPH" "$GOLDEN/font/fontref.json" > "$OUT/font.log" 2>&1; then
    pass "all 380 glyph records match"
else
    fail "glyph mismatch -- see $OUT/font.log"
    tail -20 "$OUT/font.log" | sed 's/^/        /'
fi
fi

# --------------------------------------------------------------- memory

if want all || want rss; then
head2 "8. Idle memory (the entire point of the exercise)"
CORE="$SRC/build/default/bin/nd-core"
if [ ! -x "$CORE" ]; then
    skip "nd-core not built yet"
else
    # Measured the same way the Python baseline was, so the numbers compare.
    "$CORE" --headless --idle-measure > "$OUT/rss.log" 2>&1 &
    CORE_PID=$!
    i=0
    while [ $i -lt 30 ] && ! grep -q "idle" "$OUT/rss.log" 2>/dev/null; do
        i=$((i + 1)); sleep 0.2
    done
    if [ -r "/proc/$CORE_PID/smaps_rollup" ]; then
        RSS=$(awk '/^Rss:/{print $2}' "/proc/$CORE_PID/smaps_rollup")
        printf '        RSS %s kB (%s MB)\n' "$RSS" "$((RSS / 1024))"
        printf '        Python baseline on this host: ~20800 kB (20.8 MB)\n'
        if [ "$RSS" -lt 9216 ]; then
            pass "under the 9 MB target"
        else
            fail "over the 9 MB target"
        fi
    else
        skip "could not read smaps_rollup (process exited?)"
    fi
    kill "$CORE_PID" 2>/dev/null
    wait "$CORE_PID" 2>/dev/null
fi
fi

# --------------------------------------------------------------- summary

head2 "Summary"
if [ "$FAILURES" -eq 0 ]; then
    printf '  \033[32mall checks passed\033[0m\n\n'
else
    printf '  \033[31m%d check(s) failed\033[0m -- logs in %s\n\n' "$FAILURES" "$OUT"
fi
exit "$FAILURES"
