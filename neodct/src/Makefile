# NeoDCT -- the C build.
#
# Plain make on purpose. There is no autotools, no cmake and no generator:
# Buildroot invokes this directly (see buildroot/package/neodct/neodct.mk) and
# a developer invokes the same file by hand. One build description, two users.
#
#   make                 build everything that exists
#   make test            build and run the host unit tests
#   make ASAN=1          add -fsanitize=address,undefined
#   make TSAN=1          add -fsanitize=thread
#   make install         DESTDIR=... into the target layout
#   make clean           remove this variant's build directory
#   make distclean       remove all of them
#   make format          clang-format every source file in place
#   make V=1 ...         echo the commands
#
# ============ WHY THE SOURCE LISTS ARE GLOBBED ============
#
# Ten agents are writing modules into lib/, core/, apps/ and test/unit/ at the
# same time. A hand-maintained file list would be a merge conflict on every
# commit and would silently drop a module whose author forgot to add it. So
# every directory is a wildcard, and a component with no sources yet is
# skipped with a note rather than failing the build. That is what lets the
# tree compile today, when most of it does not exist.

# ------------------------------------------------------------------ #
# Toolchain
# ------------------------------------------------------------------ #

CC         ?= cc
PKG_CONFIG ?= pkg-config
INSTALL    ?= install

# Buildroot passes NEODCT_CFLAGS/NEODCT_LDFLAGS with the cross toolchain's own
# flags already in them; a host build leaves them empty.
NEODCT_CFLAGS  ?=
NEODCT_LDFLAGS ?=

# ------------------------------------------------------------------ #
# Warning flags -- CODING-STANDARDS.md section 6, verbatim and non-negotiable
# ------------------------------------------------------------------ #
#
# -Wconversion is the annoying one and it is on deliberately: implicit
# narrowing on 32-bit ARM is a real source of pixel-offset bugs that do not
# reproduce on a 64-bit desktop, which is exactly where these tests run.
# -Wvla backs up the "nothing sized by input goes on the stack" rule.

WARNINGS := \
	-std=c11 \
	-Wall \
	-Wextra \
	-Werror \
	-Wshadow \
	-Wconversion \
	-Wstrict-prototypes \
	-Wmissing-prototypes \
	-Wvla

# ------------------------------------------------------------------ #
# Sanitizers -- each gets its own build directory, so switching between them
# needs no clean and a stale object can never be linked into the wrong variant.
# ------------------------------------------------------------------ #

VARIANT := default
SANFLAGS :=

ifeq ($(ASAN),1)
  ifeq ($(TSAN),1)
    $(error ASAN and TSAN cannot be combined -- they instrument the same code paths)
  endif
  VARIANT  := asan
  # -fno-omit-frame-pointer so the reports name real functions, and -O1 so the
  # inliner does not eat the frames they name.
  SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-common -O1 -g3
endif

ifeq ($(TSAN),1)
  VARIANT  := tsan
  SANFLAGS := -fsanitize=thread -fno-omit-frame-pointer -O1 -g3
endif

OPTFLAGS := -O2 -g
ifneq ($(SANFLAGS),)
  OPTFLAGS :=
endif

# ------------------------------------------------------------------ #
# Dependencies
# ------------------------------------------------------------------ #

# The two update-system libraries. NEITHER is new on the phone:
#
#   libcrypto  the signature verifier (lib/nd_signing.c). libcrypto.so.3 is
#              already in the image because OpenSSH links it. See
#              nd_signing.h for why the Python's hand-rolled RSA is not
#              ported instead.
#   zlib       the .ndsw reader's inflate (lib/nd_package.c).
#
# Both were already in NEODCT_DEPENDENCIES in buildroot/package/neodct/
# neodct.mk before either was linked here.
PKG_DEPS := freetype2 libpng libjpeg sqlite3 libcrypto zlib

PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKG_DEPS) 2>/dev/null)
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs   $(PKG_DEPS) 2>/dev/null)

ifeq ($(strip $(PKG_LIBS)),)
  $(error pkg-config could not find one of: $(PKG_DEPS). Install the -dev packages.)
endif

# ------------------------------------------------------------------ #
# Flags
# ------------------------------------------------------------------ #

# _GNU_SOURCE because -std=c11 defines __STRICT_ANSI__, which hides realpath,
# strdup, pipe2 and most of what a POSIX program needs.
CPPFLAGS_ALL := -Iinclude -D_GNU_SOURCE -MMD -MP $(PKG_CFLAGS)

# -ffunction-sections/-fdata-sections plus --gc-sections at link keeps the
# unused half of libneodct out of every app's mapping, which is the whole point
# of the shared library on a 53 MB device.
CFLAGS_ALL := $(WARNINGS) $(OPTFLAGS) $(SANFLAGS) -fPIC \
	-ffunction-sections -fdata-sections \
	$(NEODCT_CFLAGS) $(CFLAGS)

# $ORIGIN/../lib resolves to build/<variant>/lib on the host and to
# /NeoDCT/System/lib on the target, because both keep bin/ and lib/ as
# siblings. The absolute path is kept as well: the rootfs is a read-only
# squashfs and ldconfig cannot rebuild its cache at runtime.
RPATH_FLAGS := -Wl,-rpath,'$$ORIGIN/../lib' -Wl,-rpath,/NeoDCT/System/lib

LDFLAGS_ALL := $(SANFLAGS) -Wl,--gc-sections $(RPATH_FLAGS) $(NEODCT_LDFLAGS) $(LDFLAGS)

# ------------------------------------------------------------------ #
# Layout
# ------------------------------------------------------------------ #

BUILD   := build/$(VARIANT)
OBJDIR  := $(BUILD)/obj
BINDIR  := $(BUILD)/bin
LIBDIR  := $(BUILD)/lib
TESTDIR := $(BUILD)/test

LIBNAME := libneodct.so
LIB     := $(LIBDIR)/$(LIBNAME)

DESTDIR ?=
PREFIX  ?= /NeoDCT/System

# The reference set captured from the Python build. The oracle for every
# rendering test; see neodct/tests/golden/README.md.
GOLDEN := $(abspath ../tests/golden)

# ------------------------------------------------------------------ #
# Sources -- all globbed. See the note at the top.
# ------------------------------------------------------------------ #

LIB_SRCS    := $(wildcard lib/*.c)
CORE_SRCS   := $(wildcard core/*.c)
APPRUN_SRCS := $(wildcard apprun/*.c)
MEDIA_SRCS  := $(wildcard mediawidget/*.c)
SHOOT_SRCS  := $(wildcard tools/nd_shoot*.c)
SELF_SRCS   := $(wildcard tools/nd_selftest*.c)
# A build-host tool, never installed: see its header.
DLGFIT_SRCS := $(wildcard tools/nd_dialogfit.c)
VERIFY_SRCS := $(wildcard tools/nd_verify*.c)
PANIC_SRCS  := $(wildcard tools/nd_panic*.c)
# The initramfs progress bar. Two layers on purpose (see nd_bootfb.h): the
# framebuffer primitive, which knows nothing about updates and which the
# recovery-UI work is welcome to take, and the bar itself.
BOOTFB_SRCS  := $(wildcard tools/nd_bootfb.c tools/nd_bootfont.c)
BOOTBAR_SRCS := $(wildcard tools/nd_bootbar.c tools/nd_bootbar_ui.c)
# A build-host generator, never installed: it renders the real font.ttf into
# the committed 1-bit tables in tools/nd_bootfont.c.
GENFONT_SRCS := $(wildcard tools/gen_bootfont.c)
DISPLAYD_SRCS := $(wildcard displayd/*.c)
RECUI_SRCS  := $(wildcard recovery/*.c)
TEST_SRCS   := $(wildcard test/unit/*.c)

# Linked into every test binary and test app, never into anything that ships:
# a constructor that disarms the real halt and refuses to run outside the
# harness. test/harness/nd_testguard.c says why, at length.
HARNESS_OBJ := $(OBJDIR)/test/harness/nd_testguard.o
SANDBOX     := test/harness/sandbox.sh
RUN_TESTS   := test/harness/run-tests.sh

# An app is any directory under apps/ that contains at least one .c file.
APP_DIRS := $(sort $(dir $(wildcard apps/*/*.c)))

LIB_OBJS    := $(patsubst %.c,$(OBJDIR)/%.o,$(LIB_SRCS))
CORE_OBJS   := $(patsubst %.c,$(OBJDIR)/%.o,$(CORE_SRCS))
APPRUN_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(APPRUN_SRCS))
MEDIA_OBJS  := $(patsubst %.c,$(OBJDIR)/%.o,$(MEDIA_SRCS))
SHOOT_OBJS  := $(patsubst %.c,$(OBJDIR)/%.o,$(SHOOT_SRCS))
DLGFIT_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(DLGFIT_SRCS))
SELF_OBJS   := $(patsubst %.c,$(OBJDIR)/%.o,$(SELF_SRCS))
VERIFY_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(VERIFY_SRCS))
PANIC_OBJS  := $(patsubst %.c,$(OBJDIR)/%.o,$(PANIC_SRCS))
BOOTFB_OBJS  := $(patsubst %.c,$(OBJDIR)/%.o,$(BOOTFB_SRCS))
BOOTBAR_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(BOOTBAR_SRCS))
GENFONT_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(GENFONT_SRCS))
# Everything in nd-bootbar except its main(), which is what test_bootbar
# links against.
BOOTBAR_LIB_OBJS := $(filter-out $(OBJDIR)/tools/nd_bootbar.o,$(BOOTBAR_OBJS)) $(BOOTFB_OBJS)
DISPLAYD_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(DISPLAYD_SRCS))
RECUI_OBJS  := $(patsubst %.c,$(OBJDIR)/%.o,$(RECUI_SRCS))

# The recovery UI's library half, without main(). test_recui links these
# directly, which is the only way to reach a program that deliberately cannot
# be linked against libneodct.
RECUI_LIB_OBJS := $(filter-out $(OBJDIR)/recovery/nd_recui.o,$(RECUI_OBJS))

TEST_BINS := $(patsubst test/unit/%.c,$(TESTDIR)/%,$(TEST_SRCS))
APP_SOS   := $(foreach d,$(APP_DIRS),$(BUILD)/$(patsubst apps/%/,apps/%,$(d))/app.so)

# Apps that exist only to be launched BY a test -- the deliberately faulting
# one that proves the crash screen appears and the core survives. Built like
# any other app.so, never installed, and kept out of apps/ so that no image
# can ever ship a program whose whole job is to dereference NULL.
TEST_APP_DIRS := $(sort $(dir $(wildcard test/apps/*/*.c)))
TEST_APP_SOS  := $(foreach d,$(TEST_APP_DIRS),\
	$(TESTDIR)/$(patsubst test/apps/%/,apps/%,$(d))/app.so)

# Only ask for a binary whose sources exist.
TARGETS := $(if $(LIB_SRCS),$(LIB))
TARGETS += $(if $(CORE_SRCS),$(BINDIR)/nd-core)
TARGETS += $(if $(APPRUN_SRCS),$(BINDIR)/nd-apprun)
TARGETS += $(if $(MEDIA_SRCS),$(BINDIR)/neodct-play)
TARGETS += $(if $(SHOOT_SRCS),$(BINDIR)/nd-shoot)
TARGETS += $(if $(DLGFIT_SRCS),$(BINDIR)/nd-dialogfit)
TARGETS += $(if $(SELF_SRCS),$(BINDIR)/nd-selftest)
TARGETS += $(if $(VERIFY_SRCS),$(BINDIR)/nd-verify)
TARGETS += $(if $(PANIC_SRCS),$(BINDIR)/nd-panic)
TARGETS += $(if $(BOOTBAR_SRCS),$(BINDIR)/nd-bootbar)
TARGETS += $(if $(GENFONT_SRCS),$(BINDIR)/gen-bootfont)
TARGETS += $(if $(DISPLAYD_SRCS),$(BINDIR)/neodct_displayd)
TARGETS += $(if $(RECUI_SRCS),$(BINDIR)/nd-recui)
TARGETS += $(APP_SOS)

# ------------------------------------------------------------------ #
# Noise control
# ------------------------------------------------------------------ #

ifeq ($(V),1)
  Q :=
  say = @true
else
  Q := @
  say = @printf '  %-7s %s\n'
endif

# ------------------------------------------------------------------ #
# Rules
# ------------------------------------------------------------------ #

.PHONY: all test test-one clean distclean install format skipped help
.DEFAULT_GOAL := all

all: $(TARGETS) skipped

# Says out loud what is not being built yet, so nobody spends an afternoon
# wondering why nd-shoot is missing.
skipped:
	$(Q)$(if $(LIB_SRCS),,printf '  %-7s %s\n' 'SKIP' '$(LIBNAME) (no lib/*.c yet)';)
	$(Q)$(if $(CORE_SRCS),,printf '  %-7s %s\n' 'SKIP' 'nd-core (no core/*.c yet)';)
	$(Q)$(if $(APPRUN_SRCS),,printf '  %-7s %s\n' 'SKIP' 'nd-apprun (no apprun/*.c yet)';)
	$(Q)$(if $(SHOOT_SRCS),,printf '  %-7s %s\n' 'SKIP' 'nd-shoot (no tools/nd_shoot*.c yet)';)
	$(Q)$(if $(SELF_SRCS),,printf '  %-7s %s\n' 'SKIP' 'nd-selftest (no tools/nd_selftest*.c yet)';)
	$(Q)$(if $(VERIFY_SRCS),,printf '  %-7s %s\n' 'SKIP' 'nd-verify (no tools/nd_verify*.c yet)';)
	$(Q)$(if $(PANIC_SRCS),,printf '  %-7s %s\n' 'SKIP' 'nd-panic (no tools/nd_panic*.c yet)';)
	$(Q)$(if $(RECUI_SRCS),,printf '  %-7s %s\n' 'SKIP' 'nd-recui (no recovery/*.c yet)';)
	$(Q)$(if $(BOOTBAR_SRCS),,printf '  %-7s %s\n' 'SKIP' 'nd-bootbar (no tools/nd_bootbar*.c yet)';)
	$(Q)$(if $(TEST_SRCS),,printf '  %-7s %s\n' 'SKIP' 'unit tests (no test/unit/*.c yet)';)

$(OBJDIR)/%.o: %.c
	$(Q)mkdir -p $(dir $@)
	$(say) CC $<
	$(Q)$(CC) $(CPPFLAGS_ALL) $(CFLAGS_ALL) -c -o $@ $<

$(LIB): $(LIB_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)$(CC) -shared -Wl,-soname,$(LIBNAME) -o $@ $^ $(LDFLAGS_ALL) $(PKG_LIBS) -lm

# Everything links against libneodct: the core, the app stub, the tools and
# every test. One mapping, shared by every process on the phone.
define LINK_BIN
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)$(CC) -o $@ $^ -L$(LIBDIR) -lneodct $(LDFLAGS_ALL) $(PKG_LIBS) -lm
endef

$(BINDIR)/nd-core: $(CORE_OBJS) $(LIB)
	$(LINK_BIN)

# -rdynamic so an app's .so can call back into symbols nd-apprun itself
# exported, and -ldl for the dlopen it exists to perform.
$(BINDIR)/nd-apprun: $(APPRUN_OBJS) $(LIB)
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)$(CC) -rdynamic -o $@ $(APPRUN_OBJS) -L$(LIBDIR) -lneodct $(LDFLAGS_ALL) $(PKG_LIBS) -ldl -lm


# ---- neodct_displayd: the ST7789 SPI panel daemon ------------------
#
# Built from source on every `make`, and no longer shipped as a committed
# binary. That blob was ARM, dynamically linked, and glibc-only -- under musl
# its interpreter (/lib/ld-linux-armhf.so.3) does not exist and the kernel
# cannot exec it at all. mkinitramfs.py copies the same file into the
# initramfs, so the blob broke the boot-time update applier too.
#
# It links nothing of ours: it is a standalone SPI driver that talks to
# /dev/spidev and /dev/fb0 and predates this port. So it does NOT link
# libneodct, and it is built WITHOUT -Wconversion. The 24 conversion warnings
# are ordinary driver byte-arithmetic in code that is proven on real hardware,
# and silencing them by editing a working panel driver would risk more than it
# would buy. Everything else in the warning set, including -Werror, applies.
DISPLAYD_CFLAGS := $(filter-out -Wconversion -Wsign-conversion,$(CFLAGS))

$(OBJDIR)/displayd/%.o: displayd/%.c
	$(Q)mkdir -p $(dir $@)
	$(Q)printf '  %-7s %s\n' 'CC' '$<'
	$(Q)$(CC) $(DISPLAYD_CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(BINDIR)/neodct_displayd: $(DISPLAYD_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(Q)printf '  %-7s %s\n' 'LINK' '$@'
	$(Q)$(CC) $(DISPLAYD_OBJS) -o $@ $(LDFLAGS)

# ---- nd-recui: the on-screen recovery UI ---------------------------
#
# The second program here that does NOT link libneodct, and for a stronger
# reason than nd-verify's: it runs inside the initramfs, where /NeoDCT/System
# -- the squashfs libneodct, font.ttf, the icons and the wallpapers all live
# on -- is by definition not mounted, because failing to mount it is why
# recovery is running at all.
#
# Built WITHOUT -Iinclude and without $(PKG_CFLAGS), and that is the
# enforcement mechanism rather than a tidiness preference: a stray
# `#include "nd_ui.h"` has to fail to compile, not quietly pull in freetype
# and produce a binary the initramfs cannot run.
#
# Dynamic, not static, which is where it differs from nd-verify: musl's
# libc.so is already in the initramfs for busybox, so a dynamic link costs
# zero new bytes, while nd-verify is static only to avoid shipping a 5.8 MB
# libcrypto.so beside it.
#
# The full warning set including -Wconversion applies. displayd is exempt
# because it is a proven hardware driver predating the port; this is new code
# and has no such claim.
RECUI_CFLAGS := $(WARNINGS) $(OPTFLAGS) $(SANFLAGS) -D_GNU_SOURCE -MMD -MP \
	$(NEODCT_CFLAGS) $(CFLAGS)

$(OBJDIR)/recovery/%.o: recovery/%.c
	$(Q)mkdir -p $(dir $@)
	$(say) CC $<
	$(Q)$(CC) $(RECUI_CFLAGS) -c -o $@ $<

$(BINDIR)/nd-recui: $(RECUI_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)$(CC) -o $@ $^ $(SANFLAGS) $(NEODCT_LDFLAGS) $(LDFLAGS)

# neodct-play: the MediaWidget CLI. NetSurf execs this by absolute path
# when a <video> is clicked, so where it installs is part of the contract
# with the browser -- see the install rule and nd_media.h.
$(BINDIR)/neodct-play: $(MEDIA_OBJS) $(LIB)
	$(LINK_BIN)

$(BINDIR)/nd-shoot: $(SHOOT_OBJS) $(LIB)
	$(LINK_BIN)

$(BINDIR)/nd-selftest: $(SELF_OBJS) $(LIB)
	$(LINK_BIN)

$(BINDIR)/nd-dialogfit: $(DLGFIT_OBJS) $(LIB)
	$(LINK_BIN)

# ---- nd-verify: the initramfs signature check ----------------------
#
# The one binary here that does NOT link libneodct, and the one that is
# linked statically. Both are deliberate and nd_verify.c's header explains
# why: it runs inside the initramfs, before /NeoDCT exists, and a dynamic
# build would mean shipping libcrypto.so.3 (5.8 MB) beside it instead of a
# self-contained 4.3 MB. libneodct would drag in freetype, sqlite, libpng
# and libjpeg for a program that only needs RSA and SHA-256.
#
# Static is attempted and NOT required: a developer host without a static
# libcrypto still gets a working nd-verify for `make test`, it just cannot
# go in an initramfs. Buildroot's staging always has libcrypto.a, so the
# real image always gets the static one. The fallback says so out loud
# rather than silently producing a binary the boot cannot use.
VERIFY_LIBS := $(shell $(PKG_CONFIG) --libs libcrypto 2>/dev/null)

$(BINDIR)/nd-verify: $(VERIFY_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)if $(CC) -static -o $@.static $^ $(LDFLAGS_ALL) $(VERIFY_LIBS) \
	        > $@.link.log 2>&1; then \
	    mv -f $@.static $@; rm -f $@.link.log; \
	else \
	    rm -f $@.static; \
	    printf '  %-7s %s\n' 'NOTE' 'nd-verify: static link failed (see $@.link.log); linking dynamically'; \
	    $(CC) -o $@ $^ $(LDFLAGS_ALL) $(VERIFY_LIBS); \
	fi
# nd-panic: the core-crash screen. /bin/run_neodct.sh runs it by absolute
# path after nd-core dies, so where it installs is part of the contract with
# the boot script -- see the install rule and nd_panic.h.
$(BINDIR)/nd-panic: $(PANIC_OBJS) $(LIB)
	$(LINK_BIN)

# ---- nd-bootbar: the initramfs install-progress screen ------------------
#
# The second binary here that does NOT link libneodct, and for the same
# reason nd-verify does not: it runs inside the initramfs, before /NeoDCT
# exists. libneodct would drag in freetype, sqlite, libpng and libjpeg for a
# program that draws two rectangles and some 1-bit glyphs.
#
# Static is attempted and NOT required, exactly as for nd-verify: a developer
# host without a static libc still gets a working nd-bootbar for `make test`,
# it just cannot go in an initramfs. Unlike nd-verify this links nothing but
# libc, so statically against musl it is tens of kilobytes rather than 4.3 MB.
$(BINDIR)/nd-bootbar: $(BOOTBAR_OBJS) $(BOOTFB_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)if $(CC) -static -o $@.static $^ $(LDFLAGS_ALL) > $@.link.log 2>&1; then \
	    mv -f $@.static $@; rm -f $@.link.log; \
	else \
	    rm -f $@.static; \
	    printf '  %-7s %s\n' 'NOTE' 'nd-bootbar: static link failed (see $@.link.log); linking dynamically'; \
	    $(CC) -o $@ $^ $(LDFLAGS_ALL); \
	fi

# gen-bootfont runs on the BUILD HOST and is never installed. It links
# libneodct on purpose: the boot screen's glyph metrics have to be the
# phone's glyph metrics, so they come out of the same nd_font.
$(BINDIR)/gen-bootfont: $(GENFONT_OBJS) $(LIB)
	$(LINK_BIN)

# nd_bootbar.h and nd_bootfb.h live in tools/ and NOT in include/, on
# purpose: nothing in libneodct may depend on the initramfs's private
# drawing layer. test_bootbar is the one translation unit that needs both
# sides, so it gets the extra include path and nobody else does.
$(OBJDIR)/test/unit/test_bootbar.o: CPPFLAGS_ALL += -Itools

# test_bootbar links BOTH libneodct and the boot bar: its whole job is to
# hold the two against each other, asserting that nd_bootbar's hard-coded
# boxes are the ones nd_progress_init() derives on a real 240x175 nd_ui.
$(TESTDIR)/test_bootbar: $(OBJDIR)/test/unit/test_bootbar.o $(BOOTBAR_LIB_OBJS) $(HARNESS_OBJ) $(LIB)
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)$(CC) $^ -o $@ $(LDFLAGS_ALL) $(PKG_LIBS)

# One binary per test file, each with its own main(). No test framework: a
# test that needs a framework to express itself is usually testing the
# framework.
$(TESTDIR)/%: $(OBJDIR)/test/unit/%.o $(HARNESS_OBJ) $(LIB)
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)$(CC) -o $@ $< $(HARNESS_OBJ) -L$(LIBDIR) -lneodct $(LDFLAGS_ALL) $(PKG_LIBS) -lm

# test_recui is the exception to the rule above, because nd-recui is the
# exception to the build: it cannot be linked against libneodct, so its
# objects have to come in directly. nd_recui.o is left out -- it has main().
# libneodct still comes along, for platform_test.h and for the PNG writer the
# test dumps its frames through; the two halves never call each other.
$(TESTDIR)/test_recui: $(OBJDIR)/test/unit/test_recui.o $(RECUI_LIB_OBJS) $(HARNESS_OBJ) $(LIB)
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)$(CC) -o $@ $(filter %.o,$^) -L$(LIBDIR) -lneodct $(LDFLAGS_ALL) $(PKG_LIBS) -lm

# App shared objects. Each apps/<Name>/ becomes <Name>/app.so, installed
# beside the manifest.json and icon.png that are already there.
define APP_RULE
$(BUILD)/apps/$(1)/app.so: $$(patsubst %.c,$$(OBJDIR)/%.o,$$(wildcard apps/$(1)/*.c)) $$(LIB)
	$$(Q)mkdir -p $$(dir $$@)
	$$(say) LINK $$@
	$$(Q)$$(CC) -shared -o $$@ $$(filter %.o,$$^) -L$$(LIBDIR) -lneodct $$(LDFLAGS_ALL) $$(PKG_LIBS) -lm
endef
$(foreach d,$(APP_DIRS),$(eval $(call APP_RULE,$(patsubst apps/%/,%,$(d)))))

define TEST_APP_RULE
$(TESTDIR)/apps/$(1)/app.so: $$(patsubst %.c,$$(OBJDIR)/%.o,$$(wildcard test/apps/$(1)/*.c)) $$(HARNESS_OBJ) $$(LIB)
	$$(Q)mkdir -p $$(dir $$@)
	$$(say) LINK $$@
	$$(Q)$$(CC) -shared -o $$@ $$(filter %.o,$$^) -L$$(LIBDIR) -lneodct $$(LDFLAGS_ALL) $$(PKG_LIBS) -lm
endef
$(foreach d,$(TEST_APP_DIRS),$(eval $(call TEST_APP_RULE,$(patsubst test/apps/%/,%,$(d)))))

# ------------------------------------------------------------------ #
# Tests
# ------------------------------------------------------------------ #
#
# THE SUITE RUNS IN A SANDBOX, AND ONLY THERE. On 2026-08-31 and again on
# 2026-09-04 a test reached poweroff(8) and switched off the workstation
# running it -- a test process on a desktop has the rights of the person at
# the keyboard. So `make test` hands the loop to test/harness/sandbox.sh
# (bubblewrap: no D-Bus, no network, a minimal /dev, everything but the
# checkout read-only, a private /tmp under build/) and test/harness/
# run-tests.sh puts fake poweroff/reboot/systemctl on $PATH and fails the run
# if one is ever reached. Every test binary also carries
# test/harness/nd_testguard.c, which disarms the halt in the library and
# refuses to start outside the harness -- so `build/*/test/test_x` run by
# hand stops with a message pointing at `make test-one T=test_x`.
#
# NEODCT_ROOT points the whole path layer at a scratch directory, so a test
# that writes settings.prop cannot touch a developer's real /NeoDCT -- and on
# a machine that has no /NeoDCT at all, which is every machine this runs on,
# the tests still exercise the real code paths. run-tests.sh sets it.
#
# NEODCT_GOLDEN is the absolute path to the reference set, so a test can find
# the oracle without knowing where it was invoked from. Prefer it over a
# relative path, and note it is NOT under NEODCT_ROOT -- open it with plain
# fopen, not nd_path_resolve().
#
# Every unit test must pass WITH NO ARGUMENTS. The acceptance gate
# (neodct/tools/verify-c-build.sh) additionally runs some of them with an
# explicit reference path; treat that as an override, not as the only way in.
#
# NEODCT_TEST_SANDBOX=none opts out of the container for a disposable VM or
# CI box that has nothing to lose; the fakes and the guard still apply.

TEST_ENV := NEODCT_GOLDEN=$(GOLDEN) LD_LIBRARY_PATH=$(LIBDIR) NEODCT_SANDBOX_TMP=$(abspath $(BUILD)/tmp)

test: all $(TEST_BINS) $(TEST_APP_SOS)
	$(Q)if [ -z "$(strip $(TEST_BINS))" ]; then \
	    printf '  %-7s %s\n' 'SKIP' 'make test: no test/unit/*.c yet'; exit 0; \
	fi; \
	$(TEST_ENV) $(SANDBOX) $(RUN_TESTS) $(TEST_BINS)

# One binary, the same way: make test-one T=test_modem
test-one: all $(TESTDIR)/$(T) $(TEST_APP_SOS)
	$(Q)if [ -z "$(T)" ]; then echo "usage: make test-one T=test_name"; exit 2; fi; \
	$(TEST_ENV) $(SANDBOX) $(RUN_TESTS) $(TESTDIR)/$(T)

# ------------------------------------------------------------------ #
# Install -- the target layout, whose absolute paths are load-bearing
# ------------------------------------------------------------------ #

# ---- where the one stub app.so goes -------------------------------
#
# SESSION-SCOPE.md keeps all twenty-four apps in place with their real
# manifest.json and icon.png, and gives each of them the SAME app.so: a
# MessageDialog reading "This application has not been implemented yet."
# One binary, twenty-four hard-linked-in-spirit copies, so the menu is
# complete and every icon shows.
#
# The names are the overlay's directory names, and the split is where the
# overlay puts them: engineering apps live under engineering/apps and only
# appear in the menu when engineering mode is on.
STUB_STOCK_APPS := \
	Browser Calculator Calendar CallLog Clock Games Koki Messages \
	MusicPlayer PhoneBook Power Settings Tones Update
STUB_ENG_APPS := \
	Bluetooth Crash CubeBench Downgrade FuelGauge KeypadMapperI2C \
	LCDTest LinuxShell MicTest Modem RemoteShell Sleepy

STUB_SO := $(BUILD)/apps/Stub/app.so

install: all
	$(Q)$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/bin
	$(Q)$(if $(LIB_SRCS),$(INSTALL) -m 0755 $(LIB) $(DESTDIR)$(PREFIX)/lib/,:)
	$(Q)$(if $(CORE_SRCS),$(INSTALL) -m 0755 $(BINDIR)/nd-core $(DESTDIR)$(PREFIX)/bin/,:)
	$(Q)$(if $(APPRUN_SRCS),$(INSTALL) -m 0755 $(BINDIR)/nd-apprun $(DESTDIR)$(PREFIX)/bin/,:)
	@# nd-selftest goes ON the phone, unlike nd-shoot, because the questions
	@# it answers only have answers there: whether eudev applied the rules,
	@# whether the vendor kernel has CONFIG_MNT_NS, and whether ndusr_ut is
	@# really shut out of the databases. It is ~65 kB against libneodct,
	@# which is already mapped, and it only reads.
	$(Q)$(if $(SELF_SRCS),$(INSTALL) -m 0755 $(BINDIR)/nd-selftest $(DESTDIR)$(PREFIX)/bin/,:)
	@# nd-panic ships. It is the only part of the UI that runs when nd-core
	@# is dead, and /bin/run_neodct.sh names it as
	@#   /NeoDCT/System/bin/nd-panic
	@# so this install is the whole reason the crash screen exists on a
	@# phone rather than only on a developer's host.
	$(Q)$(if $(PANIC_SRCS),$(INSTALL) -m 0755 $(BINDIR)/nd-panic $(DESTDIR)$(PREFIX)/bin/,:)
	@# The panel daemon keeps its historical path: /NeoDCT/System/hw/. Both
	@# etc/init.d/S90display and mkinitramfs.py's PANEL_DAEMON hardcode it,
	@# and mkinitramfs reads the built target tree rather than the overlay,
	@# so installing here is all it takes for the initramfs to keep working.
	$(Q)$(if $(DISPLAYD_SRCS),$(INSTALL) -d $(DESTDIR)$(PREFIX)/hw,:)
	$(Q)$(if $(DISPLAYD_SRCS),$(INSTALL) -m 0755 $(BINDIR)/neodct_displayd $(DESTDIR)$(PREFIX)/hw/,:)
	@# neodct-play goes where the BROWSER looks for it, not where it would
	@# be tidiest. netsurf-neodct/.../neodct_media.h has
	@#   #define NEODCT_MEDIA_PLAYER "/NeoDCT/System/core/MediaWidget/neodct-play"
	@# as a compile-time constant, so this path is a contract with a
	@# separately-built program. Move it and every <video> on the web goes
	@# back to answering "Cannot play that". input.conf ships beside it from
	@# the overlay, and nd_media.h hardcodes that path too.
	$(Q)$(if $(MEDIA_SRCS),$(INSTALL) -d $(DESTDIR)$(PREFIX)/core/MediaWidget,:)
	$(Q)$(if $(MEDIA_SRCS),$(INSTALL) -m 0755 $(BINDIR)/neodct-play \
	    $(DESTDIR)$(PREFIX)/core/MediaWidget/,:)
	$(Q)for so in $(APP_SOS); do \
	    name=$$(basename $$(dirname $$so)); \
	    [ "$$name" = Stub ] && continue; \
	    sub=apps; \
	    for e in $(STUB_ENG_APPS); do [ "$$e" = "$$name" ] && sub=engineering/apps; done; \
	    $(INSTALL) -d $(DESTDIR)$(PREFIX)/$$sub/$$name; \
	    $(INSTALL) -m 0755 $$so $(DESTDIR)$(PREFIX)/$$sub/$$name/app.so; \
	done
	$(Q)if [ -f $(STUB_SO) ]; then \
	    for name in $(STUB_STOCK_APPS); do \
	        d=$(DESTDIR)$(PREFIX)/apps/$$name; \
	        [ -f $$d/app.so ] || { $(INSTALL) -d $$d; \
	            $(INSTALL) -m 0755 $(STUB_SO) $$d/app.so; }; \
	    done; \
	    for name in $(STUB_ENG_APPS); do \
	        d=$(DESTDIR)$(PREFIX)/engineering/apps/$$name; \
	        [ -f $$d/app.so ] || { $(INSTALL) -d $$d; \
	            $(INSTALL) -m 0755 $(STUB_SO) $$d/app.so; }; \
	    done; \
	fi

# ---- boot artefacts, which do NOT belong in the rootfs -------------
#
# nd-verify is 4.3 MB of statically linked OpenSSL and its only caller is
# neodct/initramfs/ndsys-apply.sh. Installing it into the target tree would
# put those 4.3 MB into the read-only squashfs as well, where nothing would
# ever run it -- the Update app checks the same signature through libneodct,
# which is already mapped. So it goes to $(BOOTDESTDIR), which buildroot's
# NEODCT_INSTALL_IMAGES_CMDS points at $(BINARIES_DIR), beside the kernel and
# the initramfs it is packed into.
BOOTDESTDIR ?= $(BUILD)/boot

# STRIP comes from buildroot's TARGET_CONFIGURE_OPTS in the cross build and
# defaults to the host's for a plain `make`. Stripping matters here in a way
# it does not for the rootfs, which buildroot strips itself: BINARIES_DIR is
# not touched, and unstripped this binary is 5.8 MB of debug info that would
# be packed into the initramfs and, on the Luckfox, into the kernel image.
STRIP ?= strip

install-boot: all
	$(Q)$(INSTALL) -d $(BOOTDESTDIR)
	$(Q)$(if $(VERIFY_SRCS),$(INSTALL) -m 0755 $(BINDIR)/nd-verify $(BOOTDESTDIR)/,:)
	$(Q)$(if $(VERIFY_SRCS),$(STRIP) $(BOOTDESTDIR)/nd-verify 2>/dev/null || true,:)
	@# nd-recui goes here and NOT into install:, for the same reason. Its
	@# only caller is ndsys-recovery.sh inside the initramfs; a copy in the
	@# verity-covered squashfs would be bytes nothing ever runs. Stripped
	@# for the same reason too -- BINARIES_DIR is not stripped by buildroot,
	@# and on the Luckfox this is packed into the kernel image.
	$(Q)$(if $(RECUI_SRCS),$(INSTALL) -m 0755 $(BINDIR)/nd-recui $(BOOTDESTDIR)/,:)
	$(Q)$(if $(RECUI_SRCS),$(STRIP) $(BOOTDESTDIR)/nd-recui 2>/dev/null || true,:)
	$(Q)$(if $(BOOTBAR_SRCS),$(INSTALL) -m 0755 $(BINDIR)/nd-bootbar $(BOOTDESTDIR)/,:)
	$(Q)$(if $(BOOTBAR_SRCS),$(STRIP) $(BOOTDESTDIR)/nd-bootbar 2>/dev/null || true,:)

.PHONY: install-boot

# ------------------------------------------------------------------ #
# Housekeeping
# ------------------------------------------------------------------ #

format:
	$(Q)command -v clang-format >/dev/null 2>&1 || { echo "clang-format not installed"; exit 1; }
	@# Two generated sources are excluded. nd_recfont.h (mkrecfont.py) and
	@# nd_bootfont.c (the boot bar's font) are byte-compared by their tests,
	@# so reformatting either fails that test until somebody regenerates it.
	$(Q)clang-format -i $$(find include lib core apprun apps tools test recovery \
	    -name nd_recfont.h -prune -o -name nd_bootfont.c -prune -o \
	    \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null)

clean:
	$(Q)rm -rf $(BUILD)

distclean:
	$(Q)rm -rf build

help:
	@sed -n '1,30p' $(firstword $(MAKEFILE_LIST))

-include $(shell find build -name '*.d' 2>/dev/null)

.SECONDARY:
