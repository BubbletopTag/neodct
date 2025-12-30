################################################################################
#
# fbrowser
#
################################################################################

FBROWSER_VERSION = master
FBROWSER_SITE = $(call github,shownb,FBrowser,$(FBROWSER_VERSION))
FBROWSER_LICENSE = LGPL-3.0
FBROWSER_DEPENDENCIES = qt5base qt5webkit

# Install config.json and the binary
define FBROWSER_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/build/FBrowser $(TARGET_DIR)/usr/bin/FBrowser
    $(INSTALL) -D -m 0644 $(@D)/config.json $(TARGET_DIR)/usr/bin/config.json
endef

$(eval $(cmake-package))
