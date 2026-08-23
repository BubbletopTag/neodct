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

PKG_DEPS := freetype2 libpng libjpeg sqlite3

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

# ------------------------------------------------------------------ #
# Sources -- all globbed. See the note at the top.
# ------------------------------------------------------------------ #

LIB_SRCS    := $(wildcard lib/*.c)
CORE_SRCS   := $(wildcard core/*.c)
APPRUN_SRCS := $(wildcard apprun/*.c)
SHOOT_SRCS  := $(wildcard tools/nd_shoot*.c)
SELF_SRCS   := $(wildcard tools/nd_selftest*.c)
TEST_SRCS   := $(wildcard test/unit/*.c)

# An app is any directory under apps/ that contains at least one .c file.
APP_DIRS := $(sort $(dir $(wildcard apps/*/*.c)))

LIB_OBJS    := $(patsubst %.c,$(OBJDIR)/%.o,$(LIB_SRCS))
CORE_OBJS   := $(patsubst %.c,$(OBJDIR)/%.o,$(CORE_SRCS))
APPRUN_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(APPRUN_SRCS))
SHOOT_OBJS  := $(patsubst %.c,$(OBJDIR)/%.o,$(SHOOT_SRCS))
SELF_OBJS   := $(patsubst %.c,$(OBJDIR)/%.o,$(SELF_SRCS))

TEST_BINS := $(patsubst test/unit/%.c,$(TESTDIR)/%,$(TEST_SRCS))
APP_SOS   := $(foreach d,$(APP_DIRS),$(BUILD)/$(patsubst apps/%/,apps/%,$(d))/app.so)

# Only ask for a binary whose sources exist.
TARGETS := $(if $(LIB_SRCS),$(LIB))
TARGETS += $(if $(CORE_SRCS),$(BINDIR)/nd-core)
TARGETS += $(if $(APPRUN_SRCS),$(BINDIR)/nd-apprun)
TARGETS += $(if $(SHOOT_SRCS),$(BINDIR)/nd-shoot)
TARGETS += $(if $(SELF_SRCS),$(BINDIR)/nd-selftest)
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

.PHONY: all test clean distclean install format skipped help
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

$(BINDIR)/nd-shoot: $(SHOOT_OBJS) $(LIB)
	$(LINK_BIN)

$(BINDIR)/nd-selftest: $(SELF_OBJS) $(LIB)
	$(LINK_BIN)

# One binary per test file, each with its own main(). No test framework: a
# test that needs a framework to express itself is usually testing the
# framework.
$(TESTDIR)/%: $(OBJDIR)/test/unit/%.o $(LIB)
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)$(CC) -o $@ $< -L$(LIBDIR) -lneodct $(LDFLAGS_ALL) $(PKG_LIBS) -lm

# App shared objects. Each apps/<Name>/ becomes <Name>/app.so, installed
# beside the manifest.json and icon.png that are already there.
define APP_RULE
$(BUILD)/apps/$(1)/app.so: $$(patsubst %.c,$$(OBJDIR)/%.o,$$(wildcard apps/$(1)/*.c)) $$(LIB)
	$$(Q)mkdir -p $$(dir $$@)
	$$(say) LINK $$@
	$$(Q)$$(CC) -shared -o $$@ $$(filter %.o,$$^) -L$$(LIBDIR) -lneodct $$(LDFLAGS_ALL) $$(PKG_LIBS) -lm
endef
$(foreach d,$(APP_DIRS),$(eval $(call APP_RULE,$(patsubst apps/%/,%,$(d)))))

# ------------------------------------------------------------------ #
# Tests
# ------------------------------------------------------------------ #
#
# NEODCT_ROOT points the whole path layer at a scratch directory, so a test
# that writes settings.prop cannot touch a developer's real /NeoDCT -- and on
# a machine that has no /NeoDCT at all, which is every machine this runs on,
# the tests still exercise the real code paths.

test: all $(TEST_BINS)
	$(Q)if [ -z "$(strip $(TEST_BINS))" ]; then \
	    printf '  %-7s %s\n' 'SKIP' 'make test: no test/unit/*.c yet'; exit 0; \
	fi; \
	fail=0; \
	root=$$(mktemp -d); \
	for t in $(TEST_BINS); do \
	    printf '  %-7s %s\n' 'TEST' "$$t"; \
	    if ! NEODCT_ROOT=$$root NEODCT_COLOR=1 LD_LIBRARY_PATH=$(LIBDIR) $$t; then fail=1; fi; \
	done; \
	rm -rf "$$root"; \
	if [ $$fail -ne 0 ]; then echo "FAILED"; exit 1; fi; \
	echo "all tests passed"

# ------------------------------------------------------------------ #
# Install -- the target layout, whose absolute paths are load-bearing
# ------------------------------------------------------------------ #

install: all
	$(Q)$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/bin
	$(Q)$(if $(LIB_SRCS),$(INSTALL) -m 0755 $(LIB) $(DESTDIR)$(PREFIX)/lib/,:)
	$(Q)$(if $(CORE_SRCS),$(INSTALL) -m 0755 $(BINDIR)/nd-core $(DESTDIR)$(PREFIX)/bin/,:)
	$(Q)$(if $(APPRUN_SRCS),$(INSTALL) -m 0755 $(BINDIR)/nd-apprun $(DESTDIR)$(PREFIX)/bin/,:)
	$(Q)for so in $(APP_SOS); do \
	    name=$$(basename $$(dirname $$so)); \
	    $(INSTALL) -d $(DESTDIR)$(PREFIX)/apps/$$name; \
	    $(INSTALL) -m 0755 $$so $(DESTDIR)$(PREFIX)/apps/$$name/app.so; \
	done

# ------------------------------------------------------------------ #
# Housekeeping
# ------------------------------------------------------------------ #

format:
	$(Q)command -v clang-format >/dev/null 2>&1 || { echo "clang-format not installed"; exit 1; }
	$(Q)clang-format -i $$(find include lib core apprun apps tools test -name '*.c' -o -name '*.h' 2>/dev/null)

clean:
	$(Q)rm -rf $(BUILD)

distclean:
	$(Q)rm -rf build

help:
	@sed -n '1,30p' $(firstword $(MAKEFILE_LIST))

-include $(shell find build -name '*.d' 2>/dev/null)

.SECONDARY:
