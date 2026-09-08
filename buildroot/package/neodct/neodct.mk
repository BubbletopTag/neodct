################################################################################
#
# neodct
#
################################################################################

# Built from the working tree rather than a tarball: this is the project's own
# source. SITE_METHOD=local makes buildroot set NEODCT_OVERRIDE_SRCDIR and
# rsync the tree in on every build, so `make neodct-rebuild` is the whole edit
# loop -- no version bump, no hash, no download.
NEODCT_VERSION = custom
NEODCT_SITE = $(TOPDIR)/../neodct/src
NEODCT_SITE_METHOD = local
NEODCT_LICENSE = GPL-3.0

# neodct/src/build/ is the DEVELOPER'S host build tree: x86-64 objects and an
# x86-64 libneodct.so, sitting in exactly the directories the cross build wants
# to write into. `rsync -au` preserves timestamps, so without this exclusion
# make finds those objects newer than the sources it just synced, decides
# everything is up to date, and hands x86-64 .o files to the ARM linker.
# Anyone who ran `make` in neodct/src before building the image hits it.
NEODCT_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude /build

# The licence lives at the repository root, one level above NEODCT_SITE, and
# <pkg>_LICENSE_FILES is resolved against $(@D) -- the rsynced copy -- so it
# cannot be named with a relative path. Copy it in after the rsync instead.
NEODCT_LICENSE_FILES = LICENSE
define NEODCT_COPY_LICENSE
	$(INSTALL) -m 0644 $(TOPDIR)/../LICENSE $(@D)/LICENSE
endef
NEODCT_POST_RSYNC_HOOKS += NEODCT_COPY_LICENSE

# ============ THE USERS, AND WHY THEY ARE DECLARED HERE ============
#
# ndusr and ndusr_ut are what the whole privilege split rests on: without them
# nd_priv_lookup() finds nothing, nd_priv_become() is a documented no-op, and
# EVERY APP RUNS AS ROOT. Not a degraded mode -- the entire boundary, absent.
#
# They are ALSO configured the normal way, through BR2_ROOTFS_USERS_TABLES in
# the defconfig. That was not enough, and the way it failed is worth writing
# down because it will happen again to something else:
#
#   Buildroot generates output/.config from the defconfig ONCE. Adding a line
#   to the defconfig does nothing to a tree that already has a .config, and
#   `make` never mentions it. So a checkout that predates the users table
#   keeps building images with no ndusr in them, forever, and the only signal
#   is one line in a boot log. It was found with `top` on a real build,
#   showing netsurf running as root.
#
# PACKAGES_USERS is collected from every enabled package regardless of the
# .config's rootfs settings (fs/common.mk:80 prints it into
# full_users_table.txt unconditionally; the configured table on line 82 is the
# conditional one). Declaring the users at the package level therefore reaches
# a stale .config, which the defconfig cannot.
#
# ---- and it is deliberately NOT guarded ----
#
# The obvious shape is `ifeq ($(BR2_ROOTFS_USERS_TABLES),)` so the two paths
# cannot both fire. That shape assumes the reason the users are missing is a
# .config without the line -- and if that assumption is ever wrong, the guard
# switches the fallback OFF in exactly the case it was needed for. Since
# support/scripts/mkusers accepts an entry it has already seen (verified: the
# table listed twice gives rc=0 and two users, not four), being unguarded
# costs nothing and removes the assumption.
#
# ---- and it does NOT use $(file <) ----
#
# $(file <...) is GNU make 4.0. Buildroot's stated minimum is 3.81
# (buildroot/Makefile:103), where $(file <x) is not a function call at all: it
# parses as an undefined variable and expands to THE EMPTY STRING. No error,
# no users, an image where everything runs as root. $(shell) and $(subst) are
# in every make that can build buildroot at all, so the file is read with
# those: strip comments and blank lines, mark each surviving line, then turn
# the marks back into the newlines mkusers parses on.
NEODCT_USERS_TABLE = $(TOPDIR)/../neodct/configs/users-table.txt
NEODCT_USERS = $(subst |,$(sep),$(shell sed -e 's/\#.*//' -e '/^[[:space:]]*$$/d' \
	-e 's/$$/|/' $(NEODCT_USERS_TABLE) | tr -d '\n'))

# rsync -au, which is what buildroot's local-site step runs, has NO --delete.
# It copies files in and never takes one out. So a file that is deleted from
# neodct/src -- or that belongs to a branch you are no longer on -- stays in
# the build copy forever and keeps being compiled.
#
# The Makefile globs (LIB_SRCS := $(wildcard lib/*.c)), so a stale .c is not
# ignored: it is picked up as if it were part of the tree. Checking out a
# branch that adds lib/nd_calendar.c, building an image, then checking out one
# that does not have it gives a build that fails on a file the working tree
# does not contain -- and if the stale file happens to still compile, it is
# worse, because it silently SHIPS.
#
# buildroot's own rsync is not ours to change, so the pruning happens here,
# right after it. Three things must survive, and the third is the one that
# bites:
#
#   ./build      the cross build's output directory, excluded from the rsync
#                for the reason above;
#   ./LICENSE    copied in from one level above NEODCT_SITE, so it is never in
#                the source tree either;
#   ./.*         BUILDROOT'S OWN BOOKKEEPING. .stamp_configured, .stamp_built
#                and .files-list*.txt live in $(@D) alongside the sources, and
#                none of them exist in neodct/src -- so a prune that only knew
#                about the first two would delete the package's build state
#                every time it ran. Found by writing exactly that.
define NEODCT_PRUNE_STALE_SOURCES
	cd $(@D) && find . -path ./build -prune -o -name '.?*' -prune -o -type f -print | \
	while read -r f; do \
	    rel="$${f#./}"; \
	    if [ "$$rel" != LICENSE ] && [ ! -e "$(NEODCT_SITE)/$$rel" ]; then \
	        printf 'neodct: dropping stale %s (not in neodct/src)\n' "$$rel" >&2; \
	        rm -f "$$f"; \
	    fi; \
	done; \
	:
endef
NEODCT_POST_RSYNC_HOOKS += NEODCT_PRUNE_STALE_SOURCES

NEODCT_DEPENDENCIES = host-pkgconf freetype jpeg libpng sqlite zlib openssl

# Make a plain `make` notice that the source changed.
#
# buildroot's rsync-from-override-srcdir step is
#
#     $(BUILD_DIR)/%/.stamp_rsynced:            (pkg-generic.mk:222)
#
# -- a stamp file with NO prerequisites. Once it exists make treats it as up
# to date forever, so the rsync never runs again; .stamp_built is still there
# too, and a top-level `make` walks straight past this package. You edit
# neodct/src, run make, and get a byte-identical image with nothing saying
# that nothing was rebuilt. The documented fix is to run `make neodct-rebuild`
# first, but forgetting it is silent, and a silent no-op is the worst kind.
#
# So: at parse time -- which is every make in this tree -- if any source file
# is newer than the last build, drop the three stamps that gate rsync, build
# and install. That is exactly what neodct-rebuild does, triggered by the tree
# instead of by remembering. Set NEODCT_NO_AUTO_REBUILD=y to turn it off.
#
# /build is the developer's HOST build tree (see the rsync exclusion above);
# excluded here for the same reason, so that running `make` in neodct/src does
# not force a cross rebuild it has nothing to do with. -print -quit stops the
# find at the first hit, so this costs one truncated directory walk per make.
NEODCT_STAMP_DIR = $(BUILD_DIR)/neodct-$(NEODCT_VERSION)

# ============ WHICH MACHINE THIS IMAGE IS, COMPILED IN ============
#
# DECISIONS.md D2: once QEMU is armv7 the architecture stops telling a phone
# from an emulator, and /NeoDCT/platform -- a separate artefact written by a
# separate build step -- becomes the only discriminator. So the same fact is
# also compiled into libneodct, and nd_platform.c makes a disagreement between
# the two loud rather than picking one.
#
# ---- it reuses BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS and adds NO symbol ----
#
# The users block above is the record of what one forgotten defconfig line
# costs: no ndusr, nd_priv_lookup() finding nothing, every app running as
# root, found with `top` on a real build and by nothing in the tree. A
# BR2_PACKAGE_NEODCT_PLATFORM would be a second line in two defconfigs to
# forget (AGENTS.md's first gotcha is that there are two copies of every
# defconfig and that they drift), and what forgetting it would produce is a
# phone with no discriminator in it -- the exact failure this exists to
# prevent, arriving by the route this tree has already been burned by.
#
# It also decides what the runtime check is worth. From ONE symbol through ONE
# table, the file and the constant cannot disagree for any image this tree
# builds, so the check in nd_platform.c fires only on a hand-assembled or
# tampered image. From two symbols it would mostly catch a routine editing
# slip, and a check that cries wolf stops being read.
#
# ---- qstrip is mandatory ----
#
# .config quotes the value, so the raw symbol arrives as "luckfox-armv7" WITH
# the quotes and no case in platform-id.sh matches it. buildroot/Makefile:807
# does exactly this qstrip when it hands the same variable to the post-build
# script.
#
# ---- and $(lastword) is the faithful read of the script's own rule ----
#
# buildroot calls post-build scripts as
# `script TARGET_DIR $POST_SCRIPT_ARGS $POST_BUILD_SCRIPT_ARGS`, and
# post-build-system-metadata.sh takes the LAST argument for that reason. The
# last word of this variable is the same token whenever it is non-empty, and
# when it is empty that script refuses the build anyway. If anybody ever
# appends a second argument after the tag, BOTH readers stop recognising it
# and BOTH refuse -- the same rule, because it is the same script. That
# equivalence has to keep holding; do not "fix" only one side of it.
#
# The name is ND_BUILD_PLATFORM and not NEODCT_BUILD_PLATFORM because every
# NEODCT_* name in a package .mk is buildroot's namespace (NEODCT_BUILD_CMDS,
# NEODCT_INSTALL_TARGET_CMDS), and a variable that looks like a buildroot hook
# and is not one is a trap.
ND_BUILD_PLATFORM := $(shell $(TOPDIR)/../neodct/scripts/platform-id.sh \
	$(lastword $(call qstrip,$(BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS))) cdefine 2>/dev/null)

ifneq ($(NEODCT_NO_AUTO_REBUILD),y)
NEODCT_AUTO_REBUILD := $(shell \
	stamp="$(NEODCT_STAMP_DIR)/.stamp_built"; \
	[ -f "$$stamp" ] || exit 0; \
	[ -n "$$(find '$(NEODCT_SITE)' -type f -not -path '$(NEODCT_SITE)/build/*' \
		-newer "$$stamp" -print -quit 2>/dev/null)" ] || exit 0; \
	rm -f "$(NEODCT_STAMP_DIR)"/.stamp_rsynced \
	      "$(NEODCT_STAMP_DIR)"/.stamp_built \
	      "$(NEODCT_STAMP_DIR)"/.stamp_target_installed; \
	echo "neodct: source changed, will rebuild" >&2)
endif

# And the same drop when the PLATFORM changed rather than a source file.
#
# Switching a tree from luckfox to qemu touches no source, so the block above
# sees nothing; buildroot sees nothing either, because .stamp_built has no
# dependency on .config. A -D is not a prerequisite of anything, so make would
# not rebuild the object either. The result is an image whose libneodct
# carries the OTHER machine's constant with nothing in the build saying so --
# the same silent no-op the block above exists to end, and here it produces a
# phone that files its crashes as QEMU and simulates its radio.
#
# ---- AND IT IS OUTSIDE NEODCT_NO_AUTO_REBUILD DELIBERATELY ----
#
# That knob exists to skip a truncated `find` over the source tree on every
# make, and BUILDING.md invites a developer to set it for exactly that reason.
# It is not a licence to ship a mis-assembled image, and inside the guard that
# is what it became: with it set, `make luckfox_pico_mini_defconfig && make` on
# a tree whose output/ was built for QEMU re-runs the post-build script -- so
# /NeoDCT/platform becomes hw -- while libneodct keeps ND_PLATFORM_QEMU, and
# the phone comes up claiming neither machine. This check costs one `cat` of a
# one-word file, which is not what anybody turns that knob off to avoid.
#
# .stamp_ndplatform is written by NEODCT_BUILD_CMDS after a successful build.
# It is a dotfile in $(@D), which NEODCT_PRUNE_STALE_SOURCES already skips
# (-name '.?*' -prune) along with buildroot's own bookkeeping.
NEODCT_PLATFORM_REBUILD := $(shell \
	stamp="$(NEODCT_STAMP_DIR)/.stamp_ndplatform"; \
	[ -f "$$stamp" ] || exit 0; \
	[ "$$(cat "$$stamp" 2>/dev/null)" != "$(ND_BUILD_PLATFORM)" ] || exit 0; \
	rm -f "$(NEODCT_STAMP_DIR)"/.stamp_rsynced \
	      "$(NEODCT_STAMP_DIR)"/.stamp_built \
	      "$(NEODCT_STAMP_DIR)"/.stamp_target_installed; \
	echo "neodct: platform changed, will rebuild" >&2)

# -Wconversion is in CODING-STANDARDS.md because implicit narrowing on 32-bit
# ARM is a real source of pixel-offset bugs that do not reproduce on a desktop
# build. -Werror keeps it honest.
#
# --gc-sections with -ffunction-sections drops unreferenced code at link time.
# On a 64 MB device that is worth having, and it costs nothing.
#
# -ffp-contract=off is NOT a performance choice, it is what keeps the port
# verifiable. The luckfox defconfig now builds for -mfpu=neon-vfpv4, because
# the RV1103's Cortex-A7 really does have VFPv4 and NEON with Fused-MAC (the
# displayd binary that ran on the board reports both). VFPv4 brings fused
# multiply-add, and GCC's default -ffp-contract=fast will happily rewrite
# a*b+c as a single FMA that rounds ONCE instead of twice.
#
# That changes results. Measured: over 400,000 a*b+c expressions chosen to
# cancel, fused and separate disagree in 100% of cases. CubeBench's vertices
# are exactly that shape -- rotate, then project, multiply then add -- and a
# last-bit difference there moves a wireframe pixel.
#
# The 48 byte-exact reference frames were captured from CPython, which does
# not contract. Turning contraction off for our code keeps the C on the same
# side of that, so a frame that regresses means a real bug rather than a
# rounding mode. Everything else in the image -- freetype, libpng, zlib --
# still gets the full benefit of VFPv4; only the renderer opts out, and it
# does no float work in its hot paths anyway.
NEODCT_MAKE_ENV = \
	$(TARGET_CONFIGURE_OPTS) \
	PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
	ND_BUILD_PLATFORM="$(ND_BUILD_PLATFORM)" \
	NEODCT_CFLAGS="$(TARGET_CFLAGS) -std=c11 -fPIC \
		-Wall -Wextra -Werror -Wshadow -Wconversion \
		-Wstrict-prototypes -Wmissing-prototypes -Wvla \
		-ffunction-sections -fdata-sections \
		-ffp-contract=off" \
	NEODCT_LDFLAGS="$(TARGET_LDFLAGS) -Wl,--gc-sections"

# The refusal is HERE and not $(error) at parse time. Buildroot parses every
# package .mk on every make in this tree, including `make raspberrypi_defconfig`
# and every other board's build, where BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS is
# empty and none of this is anyone's business. An $(error) up there would
# brick the whole vendored tree for boards that do not build this package; in
# the recipe it fires only when this package is actually built.
#
# The stamp is written after a successful build so the parse-time clause above
# can see a value change. It is one word, and `cat` reads it back.
define NEODCT_BUILD_CMDS
	@if [ -z "$(ND_BUILD_PLATFORM)" ]; then \
	    echo "neodct: cannot map image tag \"$(lastword $(call qstrip,$(BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS)))\" to hw or qemu;" >&2; \
	    echo "neodct:   BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS must end in a tag neodct/scripts/platform-id.sh knows." >&2; \
	    echo "neodct:   Refusing to build a libneodct that cannot tell a phone from an emulator." >&2; \
	    exit 1; \
	fi
	$(NEODCT_MAKE_ENV) $(MAKE) -C $(@D)
	@mkdir -p $(NEODCT_STAMP_DIR)
	@printf '%s\n' "$(ND_BUILD_PLATFORM)" > $(NEODCT_STAMP_DIR)/.stamp_ndplatform
endef

define NEODCT_INSTALL_TARGET_CMDS
	$(NEODCT_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

# nd-verify goes to BINARIES_DIR, not to the rootfs.
#
# It is the update signature check the initramfs runs before it dd's an image
# over the system partition -- SECURITY-AUDIT.md section 3 -- and it is a
# statically linked 4 MB binary because an initramfs cannot borrow the
# rootfs's libcrypto. Nothing in the running system calls it: the Update app
# checks the same signature through libneodct, which every process already
# maps. So installing it into TARGET_DIR would put those 4 MB into the
# read-only squashfs where they would never be executed.
#
# BINARIES_DIR is where boot artefacts live, beside the kernel and the
# initramfs it gets packed into, and buildroot does not sweep it into any
# filesystem image. post-image-neodct.sh hands the path to mkinitramfs.py.
NEODCT_INSTALL_IMAGES = YES

define NEODCT_INSTALL_IMAGES_CMDS
	$(NEODCT_MAKE_ENV) $(MAKE) -C $(@D) BOOTDESTDIR=$(BINARIES_DIR) install-boot
endef

$(eval $(generic-package))
