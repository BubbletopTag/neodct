# NeoDCT core crash guard -- sourced by /bin/run_neodct.sh, and by the host
# test suite (neodct/tests/test_crashguard.py) with a fake /proc/uptime and a
# stub nd-panic standing in for the real one.
#
# Nothing here runs at source time: the caller overrides whichever NDGUARD_*
# variable it needs and then calls the functions. That split is what makes the
# policy that decides whether a phone comes back up testable without crashing
# a phone -- the same reason initramfs/ndsys-apply.sh is shaped this way.
#
# ============ WHAT THE POLICY IS ============
#
# nd-core dies. The screen shows what happened and a countdown, and the core
# starts again. Crash, restart, crash, restart forever is WORSE than the
# frozen screen this replaces: it burns the battery, it re-enumerates the
# modem and reopens every database on each pass, and it never holds still long
# enough to read. So consecutive crashes are counted and the phone stops at
# NDGUARD_MAX.
#
# "Consecutive" is decided by how long the core LIVED, not by a clock running
# beside it. A core that ran NDGUARD_HEALTHY seconds was a working phone, so
# whatever kills it afterwards starts a new streak rather than continuing the
# old one. The measurement is a /proc/uptime delta, which is monotonic and
# needs no wall clock -- and this phone has no battery-backed RTC, so wall
# clock is exactly the thing that may be wrong.
#
# The count is a shell VARIABLE in the caller's loop, never a file. The loop is
# one shell process for the life of the boot, so a variable is enough, and it
# cannot be defeated by a /NeoDCT/User that is full, read-only or unmounted --
# which is a plausible cause of the crash being counted. It also, deliberately,
# does not survive a power cycle: pulling the battery is a real repair for a
# real class of fault, and booting straight into "not restarting" without
# having tried would answer a question the owner did not ask.
#
# ============ A CLEAN EXIT IS NOT A CRASH, BUT IT IS STILL COUNTED ============
#
# nd-core exiting 0 means it was asked to stop -- normally by init, on the way
# to a poweroff that is about to take this script with it. It gets no crash
# screen. It is still counted, because an nd-core that exits 0 instantly in a
# loop spins just as hard as one that segfaults, and the guard is the only
# thing standing between that and a phone that never boots.

: "${NDGUARD_CORE:=/NeoDCT/System/bin/nd-core}"
: "${NDGUARD_MAX:=3}"
: "${NDGUARD_HEALTHY:=120}"
: "${NDGUARD_COUNTDOWN:=3}"
: "${NDGUARD_UPTIME_FILE:=/proc/uptime}"
: "${NDGUARD_PANIC:=/NeoDCT/System/bin/nd-panic}"
: "${NDGUARD_TTY:=/dev/tty0}"
: "${NDGUARD_LOG:=/dev/null}"

# guard_uptime -- whole seconds since boot, 0 when /proc is not there.
#
# CLOCK_MONOTONIC by another name. The alternative, `date +%s`, is wrong here:
# a phone with no RTC boots at the epoch and ClockService may set the time
# forwards by fifty years partway through the very run being measured, which
# would make every crash look healthy.
guard_uptime() {
    ndg_up=""
    if [ -r "$NDGUARD_UPTIME_FILE" ]; then
        read -r ndg_up ndg_rest < "$NDGUARD_UPTIME_FILE"
    fi
    ndg_up="${ndg_up%%.*}"
    case "$ndg_up" in
        "" | *[!0-9]*) ndg_up=0 ;;
    esac
    echo "$ndg_up"
}

# guard_next_count PREV RAN_SECONDS -- the new consecutive-crash count.
#
# A run that lasted NDGUARD_HEALTHY or longer resets the streak, so its death
# is crash number ONE and not zero: it is still a crash, it is just not one of
# a series.
guard_next_count() {
    if [ "$2" -ge "$NDGUARD_HEALTHY" ]; then
        echo 1
    else
        echo $(($1 + 1))
    fi
}

# guard_should_halt COUNT -- true when the phone should stop trying.
guard_should_halt() {
    [ "$1" -ge "$NDGUARD_MAX" ]
}

# guard_banner MODE STATUS COUNT -- the fallback screen, in ANSI on tty0.
#
# Reached only when nd-panic is missing or could not reach the framebuffer.
# It is the old crash handler, kept because the whole point of putting the
# policy in the shell is that the phone still restarts when the program that
# draws the pretty version does not run. It does the waiting itself, which
# nd-panic does not need the caller to do -- see THE COUNTDOWN IS WAITED OUT
# in run_neodct.sh.
guard_banner() {
    {
        printf "\033[41m\033[1;97m"
        clear
        echo "=============================="
        echo "  CORE SYSTEM CRASHED         "
        echo "=============================="
        echo " CODE: $2"
        echo " TRY:  $3 of $NDGUARD_MAX"
        if [ "$1" = halt ]; then
            echo " NOT RESTARTING"
            echo " POWER OFF AND ON"
        else
            echo " RESTARTING..."
        fi
        echo "=============================="
    } > "$NDGUARD_TTY" 2>/dev/null
    [ "$1" = halt ] || sleep "$NDGUARD_COUNTDOWN"
    return 0
}

# guard_panic MODE STATUS COUNT -- put the crash screen up. MODE is
# "restart" (draw the countdown and return when it has run out) or "halt"
# (draw the final screen and return at once).
#
# nd-panic's stderr goes to the log and NOT to the console on purpose: tty0 is
# the panel, fbcon draws on the same framebuffer nd-panic just painted, and a
# single stray log line would land on top of the Nokia.
guard_panic() {
    if [ -x "$NDGUARD_PANIC" ]; then
        if [ "$1" = halt ]; then
            "$NDGUARD_PANIC" --halt --status "$2" --crash "$3" \
                --limit "$NDGUARD_MAX" 2>> "$NDGUARD_LOG" && return 0
        else
            "$NDGUARD_PANIC" --seconds "$NDGUARD_COUNTDOWN" --status "$2" \
                --crash "$3" --limit "$NDGUARD_MAX" 2>> "$NDGUARD_LOG" && return 0
        fi
    fi
    guard_banner "$1" "$2" "$3"
}

# guard_log MESSAGE -- one line into the core log, which is where a restart
# loop's history has to live: the SCREEN can only ever show the crash that is
# happening right now.
guard_log() {
    echo "[NeoDCT] $*" >> "$NDGUARD_LOG"
}

# guard_supervise -- run NDGUARD_CORE until it stops crashing or runs out of
# tries. This is the whole policy, in one function, so the host tests can
# drive it with a stub core instead of a phone.
#
#   returns 0  something asked the phone to stop (a poweroff). The caller
#              should exit; the UI is not coming back and is not meant to.
#   returns 1  it gave up. The crash screen is on the panel, and the caller
#              should sit still rather than let anything draw over it.
#
# Leaves NDGUARD_CRASHES holding the consecutive-crash count and
# NDGUARD_STOPPING holding whether a signal arrived.
#
# ============ THE TRAP IS NOT OPTIONAL ============
#
# A poweroff signals every process at once, and nd-core answers by exiting 0.
# Without the trap this loop could get as far as starting it again before the
# shell handled its own SIGTERM, and the phone would spend its last second
# before switching off booting. The handler runs once the foreground child has
# been reaped, which is exactly where the loop wants to hear about it.
#
# It installs a trap in the CALLER's shell, because that is the only shell a
# function has. Nothing else in the boot script traps anything.
guard_supervise() {
    NDGUARD_STOPPING=0
    NDGUARD_CRASHES=0
    trap 'NDGUARD_STOPPING=1' TERM INT HUP

    while :; do
        ndg_started=$(guard_uptime)
        # Deliberately unquoted: a test overrides NDGUARD_CORE with a command
        # and its arguments, and the phone's value has no spaces in it.
        $NDGUARD_CORE 2>> "$NDGUARD_LOG"
        ndg_status=$?
        ndg_ran=$(($(guard_uptime) - ndg_started))
        [ "$ndg_ran" -ge 0 ] || ndg_ran=0

        if [ "$NDGUARD_STOPPING" -ne 0 ]; then
            guard_log "asked to stop; not restarting nd-core"
            return 0
        fi

        NDGUARD_CRASHES=$(guard_next_count "$NDGUARD_CRASHES" "$ndg_ran")
        guard_log "nd-core exited $ndg_status after ${ndg_ran}s" \
            "(crash $NDGUARD_CRASHES of $NDGUARD_MAX)"

        if guard_should_halt "$NDGUARD_CRASHES"; then
            guard_panic halt "$ndg_status" "$NDGUARD_CRASHES"
            return 1
        fi

        # A clean exit was somebody asking nd-core to stop -- normally init,
        # on the way to a poweroff that is about to take this script too. No
        # crash screen for it. It was still counted above, because an nd-core
        # that exits 0 instantly in a loop spins exactly as hard as one that
        # faults, and the guard is the only thing between that and a phone
        # that never boots.
        [ "$ndg_status" -eq 0 ] || guard_panic restart "$ndg_status" "$NDGUARD_CRASHES"
    done
}
