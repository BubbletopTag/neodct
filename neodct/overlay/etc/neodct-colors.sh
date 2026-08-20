# Shared log colours for the shell side of NeoDCT.
#
# The Python runtime paints its own "[TAG]" prefixes (System/core/logstyle.py);
# this is the same palette for the init scripts, so a tag is the same colour
# whoever printed it. Source it, do not execute it:
#
#   . /etc/neodct-colors.sh
#   log() { echo "$(nd_tag MODEM) $*" > /dev/console; }
#
# NEODCT_COLOR=0 or NO_COLOR turns it off; both are honoured by the Python
# side too, so one variable silences the whole boot.

if [ -n "${NO_COLOR:-}" ] || [ "${NEODCT_COLOR:-1}" = "0" ]; then
    ND_COLOR=""
else
    ND_COLOR=1
fi

ND_RESET=""; ND_BOLD=""
[ -n "$ND_COLOR" ] && { ND_RESET="$(printf '\033[0m')"; ND_BOLD="$(printf '\033[1m')"; }

# nd_fg <256-colour-code> -- the escape, or nothing when colour is off.
nd_fg() { [ -n "$ND_COLOR" ] && printf '\033[38;5;%dm' "$1"; }

# Must match TAG_COLOURS in System/core/logstyle.py.
nd_colour_for() {
    case "$1" in
        MODEM)          echo 39  ;;
        ndsys|UPDATE)   echo 33  ;;
        CORE|OS)        echo 46  ;;
        BATT|FUEL)      echo 226 ;;
        NOTIFY)         echo 201 ;;
        INPUT|KEYMAP)   echo 51  ;;
        SETUP)          echo 214 ;;
        Browser)        echo 141 ;;
        sdcard)         echo 180 ;;
        KERNEL)         echo 244 ;;
        CRASH|ERROR|FATAL) echo 196 ;;
        *)              echo 250 ;;
    esac
}

# nd_tag <TAG> -- a painted "[TAG]" ready to prefix a log line.
nd_tag() {
    if [ -n "$ND_COLOR" ]; then
        printf '%s%s[%s]%s' "$ND_BOLD" "$(nd_fg "$(nd_colour_for "$1")")" "$1" "$ND_RESET"
    else
        printf '[%s]' "$1"
    fi
}

# nd_rule [char] [width] -- the divider used either side of the banner.
nd_rule() {
    _c="${1:-=}"; _w="${2:-72}"; _line=""
    while [ "${#_line}" -lt "$_w" ]; do _line="$_line$_c"; done
    if [ -n "$ND_COLOR" ]; then
        printf '%s%s%s%s\n' "$ND_BOLD" "$(nd_fg 46)" "$_line" "$ND_RESET"
    else
        printf '%s\n' "$_line"
    fi
}
