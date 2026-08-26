#!/bin/sh

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

# 4. Run the UI
#
# /NeoDCT is inside the read-only squashfs, so the crash log goes to the
# user-data partition.
#
# PYTHONPYCACHEPREFIX used to be exported here, for apps that shelled out
# to python while the port was in progress. python has left the defconfigs
# and there is no interpreter on the phone to read it.
CRASH_LOG=/NeoDCT/User/logs/crash.log
mkdir -p /NeoDCT/User/logs 2>/dev/null || CRASH_LOG=/tmp/crash.log

# nd-core replaces `python3 /NeoDCT/launcher.py`. It is the same program:
# nd_main.c is launcher.py's main() followed by System/core/main.py's run().
# No LD_LIBRARY_PATH is needed -- the binary carries an RPATH of
# /NeoDCT/System/lib, which is deliberate: the rootfs is a read-only squashfs
# and ldconfig cannot rebuild its cache at runtime.
echo "[NeoDCT] Booting..." > /dev/tty0
/NeoDCT/System/bin/nd-core 2> "$CRASH_LOG"
EXIT_CODE=$?

# ==========================================================
#    CRASH HANDLER
# ==========================================================

# 5. RE-ENABLE ECHO (If you uncomment these later, make sure to target tty0)
# stty -F /dev/tty0 echo tostop
# printf "\033[?25h" > /dev/tty0

# 6. Draw the Crash Screen ONLY to tty0
# Wrapping the commands in { } lets us redirect the whole block at once
{
  printf "\033[41m\033[1;97m"
  clear
  echo "=============================="
  echo "   CRITICAL SYSTEM FAILURE    "
  echo "=============================="
  echo " CODE: $EXIT_CODE"
  echo "=============================="
} > /dev/tty0

# If you want the dev shell to show up on tty0 later:
# printf "\033[0m" > /dev/tty0
# {
#   echo ""
#   echo "Just kidding :)"
#   echo "Dev Shell Active!"
#   echo "------------------------------"
# } > /dev/tty0

# export PS1="(CRASH)# "
# exec /bin/sh < /dev/tty0 > /dev/tty0 2>&1
