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

NEODCT_DEPENDENCIES = host-pkgconf freetype jpeg libpng sqlite zlib openssl

# -Wconversion is in CODING-STANDARDS.md because implicit narrowing on 32-bit
# ARM is a real source of pixel-offset bugs that do not reproduce on a desktop
# build. -Werror keeps it honest.
#
# --gc-sections with -ffunction-sections drops unreferenced code at link time.
# On a 64 MB device that is worth having, and it costs nothing.
NEODCT_MAKE_ENV = \
	$(TARGET_CONFIGURE_OPTS) \
	PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
	NEODCT_CFLAGS="$(TARGET_CFLAGS) -std=c11 -fPIC \
		-Wall -Wextra -Werror -Wshadow -Wconversion \
		-Wstrict-prototypes -Wmissing-prototypes -Wvla \
		-ffunction-sections -fdata-sections" \
	NEODCT_LDFLAGS="$(TARGET_LDFLAGS) -Wl,--gc-sections"

define NEODCT_BUILD_CMDS
	$(NEODCT_MAKE_ENV) $(MAKE) -C $(@D)
endef

define NEODCT_INSTALL_TARGET_CMDS
	$(NEODCT_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
