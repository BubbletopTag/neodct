# Put the NeoDCT hardware helpers on PATH so they can be run by name from a
# shell (serial console, or the engineering LinuxShell app):
#
#   neodct-sdcard scan          mount a card that was just inserted
#   neodct_displayd --stats     the SPI panel driver
#
# Init scripts and udev rules deliberately still call these by absolute
# path: udev runs RUN+= programs with a minimal environment of its own and
# never sources /etc/profile.
export PATH="$PATH:/NeoDCT/System/hw"
