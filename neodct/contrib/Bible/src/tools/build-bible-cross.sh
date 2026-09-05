#!/bin/sh
# build-bible-cross.sh -- build apps/Bible/app.so for the two real targets
# without building the whole of Buildroot.
#
#   tools/build-bible-cross.sh [outdir]
#
# Produces, under outdir (default build-bible/):
#
#   qemu-aarch64/app.so     aarch64 musl, tuned for cortex-a53
#   luckfox-armv7/app.so    armv7 musl, cortex-a7 + NEON-VFPv4 + Thumb-2, hard float
#
# ============ WHY THIS EXISTS ============
#
# The normal way to get a target app.so is `make` in buildroot, which builds a
# toolchain, a kernel and four hundred packages first. That is correct and it
# takes hours. This builds the ONE shared object, in seconds, against
# standalone musl cross toolchains -- for the case where the rest of the image
# is already flashed and only the app is new.
#
# It is not a replacement for the Buildroot build. It is the fast path.
#
# ============ HOW IT AVOIDS BUILDING libneodct FOR THE TARGET ============
#
# app.so does not contain libneodct; it references it. Linking only needs a
# file that EXPORTS the right symbols, so the script generates one: it reads
# the dynamic symbol table of the host's own build/default/lib/libneodct.so
# and emits a C file defining every symbol as an empty function or a scalar.
# That stub is compiled for the target and app.so is linked against it.
#
# Two things fall out of that, both wanted:
#
#   - the link VERIFIES the app's undefined symbols against the real
#     libneodct's export list, so a symbol that does not exist is a link
#     error here rather than a dlopen failure on the phone;
#   - the .so gets a correct DT_NEEDED on libneodct.so and libz.so.1, exactly
#     as the Buildroot build produces.
#
# The stub is never installed and never leaves the build directory.
#
# ============ WHY THE PROJECT MAKEFILE IS REUSED ============
#
# The compiler flags are not repeated here. src/Makefile is invoked with CC
# and PKG_CONFIG overridden and VARIANT set to a per-target build directory,
# so -Wconversion, -Werror, the RPATH and the section-GC flags are whatever
# the Makefile says they are today and cannot drift out of step with it.
# --assume-old is what stops make deciding it should build libneodct too.
#
# apps/Bible is the only app this works for, because it is the only one whose
# includes reach nothing outside libc and zlib. Anything touching freetype,
# libpng, sqlite or libcrypto needs those headers for the target and belongs
# in a real Buildroot build.

set -e

SRC=$(cd "$(dirname "$0")/../src" && pwd)
OUT=${1:-$(cd "$(dirname "$0")/../.." && pwd)/build-bible}

# Toolchains. musl.cc publishes both prebuilt; override to point at
# Buildroot's own host/bin if you have already built one.
AARCH64_CC=${AARCH64_CC:-aarch64-linux-musl-gcc}
ARMV7_CC=${ARMV7_CC:-armv7l-linux-musleabihf-gcc}

# Matches buildroot/configs/*_defconfig. The arm line is the one that matters:
# BR2_cortex_a7 + BR2_ARM_FPU_NEON_VFPV4 + BR2_ARM_INSTRUCTIONS_THUMB2, and
# hard float because Buildroot defaults to EABIHF once an FPU is selected. The
# result is checked at the end with readelf -A.
AARCH64_FLAGS=${AARCH64_FLAGS:--mcpu=cortex-a53}
ARMV7_FLAGS=${ARMV7_FLAGS:--mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -mthumb}

HOST_LIB=$SRC/build/default/lib/libneodct.so

if [ ! -f "$HOST_LIB" ]; then
    echo "build-bible-cross: $HOST_LIB is missing -- run 'make' in neodct/src first." >&2
    echo "The host library is where the stub's symbol list comes from." >&2
    exit 1
fi

WORK=$OUT/.work
rm -rf "$WORK"
mkdir -p "$WORK/zinc"

# ---- zlib headers ---------------------------------------------------
#
# Copied into a directory of their own rather than passed as -I/usr/include:
# an -I path is searched BEFORE the sysroot, so pointing the cross compiler at
# /usr/include would have it read the host's glibc stdio.h and produce an
# object full of __isoc23_* and *_chk symbols that musl does not export. That
# is exactly the failure this whole script exists to avoid, and it is silent
# until the phone refuses to dlopen the result.
#
# zlib.h and zconf.h are ABI-neutral: every type in them resolves through the
# target compiler, and z_stream holds no type whose size the header fixes.
ZLIB_H=${ZLIB_H:-/usr/include/zlib.h}
ZCONF_H=${ZCONF_H:-/usr/include/zconf.h}
for h in "$ZLIB_H" "$ZCONF_H"; do
    [ -f "$h" ] || { echo "build-bible-cross: $h not found; set ZLIB_H/ZCONF_H" >&2; exit 1; }
    cp "$h" "$WORK/zinc/"
done

cat > "$WORK/pkg-config" <<'EOF'
#!/bin/sh
# Stands in for pkg-config. apps/Bible needs zlib out of the PKG_DEPS set and
# nothing else; src/Makefile refuses to run if --libs comes back empty.
case "$1" in
  --cflags) echo "-I$NEODCT_XZINC" ;;
  --libs)   echo "-lz" ;;
esac
EOF
chmod +x "$WORK/pkg-config"

# ---- the stubs ------------------------------------------------------

echo "  GEN     link stubs from $(basename "$HOST_LIB")"
# seen[] because a name can appear more than once in .dynsym -- the same
# symbol shows up again under a version node -- and emitting it twice is a
# redefinition error rather than a duplicate the linker folds.
readelf -sW --dyn-syms "$HOST_LIB" |
    awk '$1 ~ /:$/ && $7 != "UND" && ($5 == "GLOBAL" || $5 == "WEAK") {
             name = $8; sub(/@.*/, "", name);
             if (name == "" || (name in seen)) next;
             seen[name] = 1;
             if ($4 == "FUNC")   printf "void %s(void); void %s(void) { }\n", name, name;
             if ($4 == "OBJECT") printf "long %s;\n", name;
         }' > "$WORK/stub_neodct.c"

cat > "$WORK/stub_z.c" <<'EOF'
void inflate(void); void inflate(void) { }
void inflateEnd(void); void inflateEnd(void) { }
void inflateInit_(void); void inflateInit_(void) { }
void inflateInit2_(void); void inflateInit2_(void) { }
void inflateReset(void); void inflateReset(void) { }
void inflateSetDictionary(void); void inflateSetDictionary(void) { }
void deflate(void); void deflate(void) { }
void deflateEnd(void); void deflateEnd(void) { }
void deflateInit_(void); void deflateInit_(void) { }
void deflateInit2_(void); void deflateInit2_(void) { }
void deflateSetDictionary(void); void deflateSetDictionary(void) { }
void crc32(void); void crc32(void) { }
void adler32(void); void adler32(void) { }
void zlibVersion(void); void zlibVersion(void) { }
EOF

# ---- one target -----------------------------------------------------

build_one() {
    variant=$1
    cc=$2
    flags=$3

    command -v "$cc" >/dev/null 2>&1 || {
        echo "build-bible-cross: $cc is not on PATH." >&2
        echo "  Prebuilt musl toolchains: https://musl.cc/" >&2
        exit 1
    }

    echo "  STUB    $variant"
    mkdir -p "$SRC/build/$variant/lib"
    # shellcheck disable=SC2086
    "$cc" -shared -fPIC $flags -Wl,-soname,libneodct.so \
        -o "$SRC/build/$variant/lib/libneodct.so" "$WORK/stub_neodct.c"
    # shellcheck disable=SC2086
    "$cc" -shared -fPIC $flags -Wl,-soname,libz.so.1 \
        -o "$SRC/build/$variant/lib/libz.so.1" "$WORK/stub_z.c"
    ln -sf libz.so.1 "$SRC/build/$variant/lib/libz.so"

    ( cd "$SRC" && NEODCT_XZINC="$WORK/zinc" \
        make VARIANT="$variant" CC="$cc" PKG_CONFIG="$WORK/pkg-config" \
             NEODCT_CFLAGS="$flags" \
             --assume-old="build/$variant/lib/libneodct.so" \
             "build/$variant/apps/Bible/app.so" )

    mkdir -p "$OUT/$variant"
    cp "$SRC/build/$variant/apps/Bible/app.so" "$OUT/$variant/app.so.debug"
    "${cc%gcc}strip" --strip-unneeded \
        -o "$OUT/$variant/app.so" "$OUT/$variant/app.so.debug"

    echo "  CHECK   $variant"
    readelf -d "$OUT/$variant/app.so" | grep -E 'NEEDED|RPATH|RUNPATH' | sed 's/^/          /'
    # A glibc header leaking in shows up here and nowhere else until the phone
    # tries to load it.
    if readelf -sW --dyn-syms "$OUT/$variant/app.so" |
       grep -qE '__isoc23_|_chk$|GLIBC_'; then
        echo "build-bible-cross: $variant has glibc-only symbols -- a host header leaked in." >&2
        exit 1
    fi
    echo "          $(readelf -h "$OUT/$variant/app.so" | sed -n 's/^  Machine: *//p')"
}

build_one qemu-aarch64  "$AARCH64_CC" "$AARCH64_FLAGS"
build_one luckfox-armv7 "$ARMV7_CC"   "$ARMV7_FLAGS"

echo "  ARM     attributes:"
readelf -A "$OUT/luckfox-armv7/app.so" |
    grep -E 'CPU_arch|FP_arch|SIMD|VFP_args|THUMB' | sed 's/^/          /'

rm -rf "$WORK"
echo
echo "built into $OUT"
