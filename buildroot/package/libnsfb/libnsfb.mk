################################################################################
#
# libnsfb
#
################################################################################

LIBNSFB_VERSION = 0.2.2
LIBNSFB_SITE = http://download.netsurf-browser.org/libs/releases
LIBNSFB_SOURCE = libnsfb-$(LIBNSFB_VERSION)-src.tar.gz
LIBNSFB_INSTALL_STAGING = YES
LIBNSFB_LICENSE = MIT
LIBNSFB_LICENSE_FILES = COPYING
LIBNSFB_DEPENDENCIES = netsurf-buildsystem

# Configure: Force Linux Backend
define LIBNSFB_CONFIGURE_CMDS
    echo "override NETSURF_FB_FONTLIB := internal" > $(@D)/Makefile.config
    echo "override NETSURF_FB_FRONTEND := linux" >> $(@D)/Makefile.config
endef

# Build: The "NSSHARED" flag tells it where to find the buildsystem tools
define LIBNSFB_BUILD_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
        NSSHARED=$(STAGING_DIR)/usr/share/netsurf-buildsystem \
        PREFIX=/usr \
        COMPONENT_TYPE=lib-static
endef

# Install Staging: Again, tell it where NSSHARED is
define LIBNSFB_INSTALL_STAGING_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
        NSSHARED=$(STAGING_DIR)/usr/share/netsurf-buildsystem \
        PREFIX=$(STAGING_DIR)/usr \
        COMPONENT_TYPE=lib-static \
        install
endef

# Install Target: And once more for the target
define LIBNSFB_INSTALL_TARGET_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
        NSSHARED=$(STAGING_DIR)/usr/share/netsurf-buildsystem \
        PREFIX=$(TARGET_DIR)/usr \
        COMPONENT_TYPE=lib-static \
        install
endef

$(eval $(generic-package))
