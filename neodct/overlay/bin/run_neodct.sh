#!/bin/sh

# 1. Silence Kernel Messages
# Prevents random system logs from drawing over your UI
dmesg -n 1

# 2. DISABLE TEXT ECHO (The fix you asked for)
# -echo : Stop printing typed characters
# -tostop : Stop background processes from writing to tty (optional but good)
stty -echo -tostop

# 3. Hide the Cursor
# \033[?25l is the ANSI code to hide the blinking cursor
printf "\033[?25l"
clear

# 4. Run the UI
echo "[NeoDCT] Booting..."
python3 /NeoDCT/launcher.py 2> /NeoDCT/crash.log
EXIT_CODE=$?

# ==========================================================
#    CRASH HANDLER
# ==========================================================

# 5. RE-ENABLE ECHO
# We MUST do this, or your emergency shell will be invisible!
stty echo tostop
# Re-enable the cursor (\033[?25h)
printf "\033[?25h"

# 6. Draw the Crash Screen
printf "\033[41m\033[1;97m"
clear

echo "=============================="
echo "   CRITICAL SYSTEM FAILURE    "
echo "=============================="
echo " CODE: $EXIT_CODE"
echo "=============================="

printf "\033[0m"

echo ""
echo "Just kidding :)"
echo "Dev Shell Active!"
echo "------------------------------"

export PS1="(CRASH)# "
/bin/sh