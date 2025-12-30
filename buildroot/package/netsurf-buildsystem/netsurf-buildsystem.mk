################################################################################
#
# netsurf-buildsystem
#
################################################################################

NETSURF_BUILDSYSTEM_VERSION = 1.10
NETSURF_BUILDSYSTEM_SITE = http://download.netsurf-browser.org/libs/releases
NETSURF_BUILDSYSTEM_SOURCE = buildsystem-$(NETSURF_BUILDSYSTEM_VERSION).tar.gz
NETSURF_BUILDSYSTEM_INSTALL_STAGING = YES
NETSURF_BUILDSYSTEM_INSTALL_TARGET = NO
NETSURF_BUILDSYSTEM_LICENSE = MIT
NETSURF_BUILDSYSTEM_LICENSE_FILES = COPYING

define NETSURF_BUILDSYSTEM_INSTALL_STAGING_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) PREFIX=$(STAGING_DIR)/usr install
endef

$(eval $(generic-package))
