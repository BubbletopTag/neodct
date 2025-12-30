################################################################################
#
# fontconfig
#
################################################################################

FONTCONFIG_VERSION = 2.17.1
FONTCONFIG_SITE = https://gitlab.freedesktop.org/api/v4/projects/890/packages/generic/fontconfig/$(FONTCONFIG_VERSION)
FONTCONFIG_SOURCE = fontconfig-$(FONTCONFIG_VERSION).tar.xz
FONTCONFIG_INSTALL_STAGING = YES
FONTCONFIG_DEPENDENCIES = freetype expat host-pkgconf host-gperf \
	$(TARGET_NLS_DEPENDENCIES)
HOST_FONTCONFIG_DEPENDENCIES = host-freetype host-expat host-pkgconf \
	host-gperf host-gettext
FONTCONFIG_LICENSE = fontconfig license
FONTCONFIG_LICENSE_FILES = COPYING
FONTCONFIG_CPE_ID_VALID = YES

# package/fontconfig/fontconfig.mk

# 1. Add Dependencies
# (Keep existing ones like host-pkgconf, host-freetype, expat, etc.)
FONTCONFIG_DEPENDENCIES += host-python3 host-python-setuptools host-python-packaging

# 2. Add the Environment Block
# REPLACE '_sysconfigdata__linux_aarch64-linux-gnu' with your actual filename
FONTCONFIG_CONF_ENV += \
    SETUPTOOLS_USE_DISTUTILS=local \
    _PYTHON_SYSCONFIGDATA_NAME="_sysconfigdata__linux_aarch64-linux-gnu" \
    PYTHONPATH=$(STAGING_DIR)/usr/lib/python3.13/:$(STAGING_DIR)/usr/lib/python3.13/site-packages/:$(HOST_DIR)/lib/python3.13/site-packages/

FONTCONFIG_CONF_OPTS = \
	-Dcache-dir=/var/cache/fontconfig \
	-Dtests=disabled \
	-Ddoc=disabled

FONTCONFIG_CFLAGS = $(TARGET_CFLAGS)

# See: https://gitlab.freedesktop.org/fontconfig/fontconfig/-/issues/436
ifeq ($(BR2_DEBUG_3),y)
FONTCONFIG_CFLAGS += -g2
endif

$(eval $(meson-package))
$(eval $(host-meson-package))
