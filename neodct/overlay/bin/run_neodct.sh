#!/bin/sh

# 0. What everything below inherits.
#
# 0027 means a file the UI creates is 0640 and a directory is 0750, which is
# the layout S00userdata sets up: the group is ndusr, and "other" -- which is
# ndusr_ut, the browser and the media player -- gets nothing.
#
# It has to be here rather than in the C, because it has to cover every
# process the core forks as well as the core itself, and because the one file
# it most needs to cover is settings.prop. /NeoDCT/User is 0751: ndusr_ut
# cannot LIST the directory, but 0644 on a file whose name it can guess is
# still a file it can read, and "settings.prop" is not a hard name to guess.
#
# Anything that genuinely needs a different mode already asks for one --
# RemoteShell writes its keys 0600 by hand, and umask can only take bits
# away, never add them.
umask 0027

# 1. Silence Kernel Messages
# Prevents random system logs from drawing over your UI
dmesg -n 1

# 2. DISABLE TEXT ECHO on tty0 specifically
# -F /dev/tty0 : Forces stty to target the physical screen
# -echo : Stop printing typed characters
# -tostop : Stop background processes from writing to tty (optional but good)
stty -F /dev/tty0 -echo -tostop

# 3. Hide the Cursor and Clear the physical screen
# Redirecting these ensures they don't blank out your SSH/Serial terminal
printf "\033[?25l" > /dev/tty0
clear > /dev/tty0

# 4. Where the logs go
#
# /NeoDCT is inside the read-only squashfs, so both logs go to the user-data
# partition.
#
# CORE_LOG is nd-core's stderr. It used to be /NeoDCT/User/logs/crash.log,
# opened with a truncating `>`, and that was wrong twice over: nd_crash.c
# APPENDS app crash reports to that same file, so every boot destroyed the
# history of every application crash, and for the whole session afterwards the
# two writers held file offsets that overwrote each other. They are separate
# files now, and this one is appended to so that a restart loop keeps the
# evidence from the crash BEFORE the one on screen -- which is the crash you
# actually want when the phone is going round in circles.
#
# One boot's worth is rotated aside at startup; the crash log rotates itself.
#
# PYTHONPYCACHEPREFIX used to be exported here, for apps that shelled out
# to python while the port was in progress. python has left the defconfigs
# and there is no interpreter on the phone to read it.
CORE_LOG=/NeoDCT/User/logs/core.log
mkdir -p /NeoDCT/User/logs 2>/dev/null || CORE_LOG=/tmp/core.log
[ -f "$CORE_LOG" ] && mv -f "$CORE_LOG" "$CORE_LOG.1" 2>/dev/null

# Optional developer environment, on the WRITABLE partition. The rootfs is a
# read-only squashfs, so without this the only way to set a variable for
# nd-core is to rebuild the image -- which is a long way to go to flip a
# switch in QEMU. nd-core inherits whatever this exports, and so does every
# app it forks.
#
# The one most people want:
#
#     echo 'export NEODCT_T9=1' > /NeoDCT/User/env.sh
#
# which turns on T9 -- multi-tap, predictive, the # mode cycle and the mode
# indicator -- on a keyboard that would otherwise take the QWERTY path and
# have no modes at all. See docs/c-rewrite/spec-hw-input.md.
#
# ============ AND WHY IT IS GATED ============
#
# This is SECURITY-AUDIT.md section 4 Q5 vector 2, and it was rated Critical:
# arbitrary shell, uid 0, from writable storage, on every boot, before the UI
# starts. Anything that can write one file gets a permanent root backdoor
# that survives an update -- an update replaces the rootfs and never touches
# /NeoDCT/User.
#
# The feature is good and it stays. What it cannot be is unconditional, and
# the gate has to be something the writable partition CANNOT set, or it is
# not a gate. Two things qualify, and neither of them is a setting:
#
#   the kernel cmdline    U-Boot environment on the phone, -append in QEMU.
#                         Not reachable from a running system at all.
#   a file in the rootfs  read-only squashfs under dm-verity. Writing one
#                         means replacing the signed image.
#
# Engineering mode is deliberately NOT accepted here, even though it gates
# LinuxShell and raw AT: it lives in settings.prop on the writable partition,
# so an attacker who can drop env.sh can set it in the same breath.
#
# ============ AND HOW TO TURN IT ON, ON A PHONE ============
#
# The gate is right and the problem was that neither source was reachable on
# hardware. run_qemu.sh puts neodct.devenv=1 on -append BY DEFAULT, so every
# override works in the emulator; the phone's U-Boot bootargs carry ubi.mtd,
# neodct.sys, neodct.user, neodct.verity and neodct.rectty and nothing else,
# and no image had ever contained /etc/neodct-devenv. So NEODCT_NO_DROP --
# which nd_main.c offers as the way to keep the old root behaviour "for a
# developer bisecting something" -- was available on every target except the
# one that breaks.
#
# Two ways in now, and both of them cost a rootfs the phone must accept:
#
#   NEODCT_DEVENV_IMAGE=1 make    builds the marker into /etc, via
#                                 neodct/scripts/post-build-devenv-marker.sh.
#                                 The marker is inside the verity-covered
#                                 squashfs, so this reaches a phone already in
#                                 the field as an .ndsw and nothing else.
#   the U-Boot environment        add neodct.devenv=1 to bootargs from the
#                                 serial console. No reflash, and no way to do
#                                 it from the running system either.
#
# Neither is reachable from /NeoDCT/User, which is the property that matters.
NEODCT_DEVENV=""
if [ -e /etc/neodct-devenv ]; then
  NEODCT_DEVENV="rootfs marker"
else
  for word in $(cat /proc/cmdline 2>/dev/null); do
    case "$word" in
      neodct.devenv=1) NEODCT_DEVENV="kernel cmdline" ;;
    esac
  done
fi

if [ -r /NeoDCT/User/env.sh ]; then
  if [ -n "$NEODCT_DEVENV" ]; then
    echo "[NeoDCT] Sourcing /NeoDCT/User/env.sh ($NEODCT_DEVENV)" > /dev/tty0
    . /NeoDCT/User/env.sh
  else
    # Loud on purpose. A developer who wonders why NEODCT_T9=1 stopped
    # working needs to be told, and an owner who finds this line on a phone
    # nobody develops on has just been told something much more interesting.
    echo "[NeoDCT] IGNORING /NeoDCT/User/env.sh: no developer environment" \
      > /dev/tty0
    echo "[NeoDCT] boot with neodct.devenv=1 to enable it" > /dev/tty0
  fi
fi

# ==========================================================
#    THE CRASH GUARD
# ==========================================================
#
# The policy -- how long counts as a healthy run, how many consecutive crashes
# are allowed, what gets drawn and what waits -- lives in nd-crashguard.sh, so
# that the host test suite can drive it without crashing a phone. Read that
# file before changing any of it; the reasoning is all there.
#
# It is SOURCED, not run, so the crash count is an ordinary shell variable in
# this process. A counter in a file could be defeated by the very thing that
# crashed the core: a /NeoDCT/User that is full, read-only, or never mounted.
NDGUARD_LOG=$CORE_LOG
NDGUARD_CORE=/NeoDCT/System/bin/nd-core

# 5. Run the UI, and keep running it
#
# nd-core replaces `python3 /NeoDCT/launcher.py`. It is the same program:
# nd_main.c is launcher.py's main() followed by System/core/main.py's run().
# No LD_LIBRARY_PATH is needed -- the binary carries an RPATH of
# /NeoDCT/System/lib, which is deliberate: the rootfs is a read-only squashfs
# and ldconfig cannot rebuild its cache at runtime.
#
# The boot is announced on tty0 exactly once. tty0 IS the panel -- the
# framebuffer console draws on the same pixels nd-panic paints -- so saying it
# again after a crash would put a line of white text across the sick Nokia for
# the second it takes nd-core to reach its first frame. Every later attempt
# goes to the log instead.
echo "[NeoDCT] Booting..." > /dev/tty0

if [ -r /bin/nd-crashguard.sh ]; then
  . /bin/nd-crashguard.sh
  guard_supervise && exit 0
else
  # Without the policy there is no guard, and an unguarded loop is worse than
  # no loop: it would spin forever on a core that cannot start. So this runs
  # the UI exactly once and then says why nothing came back.
  echo "[NeoDCT] nd-crashguard.sh missing; the UI will not be restarted" > /dev/tty0
  "$NDGUARD_CORE" 2>> "$CORE_LOG"
  echo "[NeoDCT] nd-core exited $?; no crash guard, so nothing will restart it" \
    > /dev/tty0
fi

# ==========================================================
#    HALTED
# ==========================================================
#
# The screen the guard left up is the message, and it stays up: nothing below
# writes to tty0, because that would draw over it.
#
# inittab runs this script `once`, so returning from here would leave the halt
# screen on the panel anyway -- but it would also let init reap the script.
# Sitting here is the honest expression of "this phone is not coming back on
# its own", and it costs nothing: the serial gettys on ttyFIQ0 and ttyAMA0 are
# respawned by init and are still there, so a developer has a shell, the core
# log and the crash log the whole time.
echo "[NeoDCT] halted; the crash screen stays up" >> "$CORE_LOG"
while :; do
  sleep 3600
done

# If you want the dev shell to show up on tty0 later:
# printf "\033[0m" > /dev/tty0
# stty -F /dev/tty0 echo tostop
# export PS1="(CRASH)# "
# exec /bin/sh < /dev/tty0 > /dev/tty0 2>&1
