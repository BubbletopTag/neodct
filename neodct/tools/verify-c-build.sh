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

head2 "3b. Compiles and passes with the TARGET's word size and char signedness"
# The tests run on x86-64. The phone is 32-bit ARM, and it differs in two ways
# that silently change behaviour rather than failing to build:
#
#   long and pointers are 4 bytes, not 8 -- which is what -Wconversion is on
#   for, since narrowing that is invisible on a 64-bit desktop;
#
#   plain `char` is UNSIGNED on ARM and SIGNED on x86. Any `char c; if (c < 0)`
#   or comparison against a byte >= 0x80 flips meaning between the two.
#
# 32-bit x86 is not ARM -- it tolerates unaligned access where ARM faults, and
# it is a different instruction set. But it reproduces both of the above for
# free, so it catches the portability class most likely to bite before the
# cross-build does. Running the TESTS this way, not just compiling, is the
# point: a signedness flip produces wrong answers, not warnings.
if ! gcc -m32 -E -x c /dev/null >/dev/null 2>&1; then
    skip "gcc -m32 unavailable (install gcc-multilib)"
else
    if make -C "$SRC" clean >/dev/null 2>&1 && \
       make -C "$SRC" CFLAGS="-m32 -funsigned-char" LDFLAGS="-m32" -j"$(nproc)" \
            > "$OUT/m32.log" 2>&1; then
        pass "builds 32-bit with unsigned char"
    else
        # A pkg-config miss here is the 32-bit -dev packages being absent, not
        # a portability problem; say which so it is not mistaken for one.
        if grep -q "cannot find -l\|No such file or directory" "$OUT/m32.log"; then
            skip "32-bit dev libraries not installed -- compile-only check follows"
        else
            fail "32-bit build failed -- see $OUT/m32.log"
            tail -15 "$OUT/m32.log" | sed 's/^/        /'
        fi
    fi
    # Compile-only sweep, which needs no 32-bit libraries to link against.
    M32_FAIL=0
    for c in $(find "$SRC/lib" "$SRC/core" "$SRC/apprun" "$SRC/apps" "$SRC/tools" \
                    -name '*.c' 2>/dev/null); do
        if ! gcc -m32 -funsigned-char -std=c11 -c "$c" -o /dev/null -I"$SRC/include" \
             $(pkg-config --cflags freetype2 libpng sqlite3 2>/dev/null) \
             -D_GNU_SOURCE -Wall -Wextra -Werror -Wshadow -Wconversion \
             -Wstrict-prototypes -Wmissing-prototypes -Wvla -O2 -fPIC \
             >> "$OUT/m32c.log" 2>&1; then
            grep -q "fatal error:.*No such file" "$OUT/m32c.log" || \
                M32_FAIL=$((M32_FAIL + 1))
        fi
    done
    if [ "$M32_FAIL" -eq 0 ]; then
        pass "all sources compile 32-bit + unsigned char, -Wconversion clean"
    else
        fail "$M32_FAIL source(s) fail at the target's word size"
        tail -20 "$OUT/m32c.log" | sed 's/^/        /'
    fi
    make -C "$SRC" clean >/dev/null 2>&1
    make -C "$SRC" -j"$(nproc)" >/dev/null 2>&1
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
# Rebuild the DEFAULT variant *including its test binaries*. Plain `make`
# builds the libraries and binaries but not test/, so checks 6-8 below were
# finding test_font missing and reporting SKIP -- a check that silently opts
# out is worse than one that fails.
# `make test` rather than plain `make`: it builds the test binaries as well as
# the libraries, and checks 6-8 below need them. Without this they found
# test_font missing and reported SKIP -- a check that silently opts out is
# worse than one that fails, because it looks like a pass at a glance.
make -C "$SRC" clean >/dev/null 2>&1
make -C "$SRC" -j"$(nproc)" test >/dev/null 2>&1
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
        python3 neodct/tools/goldenframe.py --compare "$GOLDEN" "$OUT/frames" \
            > "$OUT/compare.log" 2>&1
        # "missing" and "pixels" mean completely different things and must not
        # be reported the same way. A frame nd-shoot deliberately skipped --
        # because its app is not ported yet -- is remaining work. A frame whose
        # PIXELS differ is a regression, and the only one of the two that
        # should ever fail this gate. Conflating them makes the port look
        # broken for its entire duration and trains everyone to ignore check 6.
        RENDERED=$(ls "$OUT/frames"/*.png 2>/dev/null | wc -l)
        PIXDIFF=$(grep -cE '[[:space:]](pixels|size)[[:space:]]' "$OUT/compare.log" || true)
        MISSING=$(grep -c 'missing' "$OUT/compare.log" || true)
        if [ "$PIXDIFF" -eq 0 ]; then
            pass "$RENDERED rendered, all byte-exact ($MISSING not ported yet)"
        else
            fail "$PIXDIFF frame(s) REGRESSED (pixels differ)"
            grep -E '[[:space:]](pixels|size)[[:space:]]' "$OUT/compare.log" \
                | head -20 | sed 's/^/        /'
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
