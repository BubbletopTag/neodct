#!/bin/sh
# cloud-test.sh -- put the phone's screen on the internet.
#
# A cloud agent can build this OS and run its tests, but it cannot LOOK at it.
# This closes that gap: an X server with no monitor, a window manager, the
# QEMU window inside it, and two ways in from outside -- a browser (noVNC over
# an ngrok HTTP tunnel, which needs nothing installed at the far end) and a
# native VNC client (over an ngrok TCP tunnel, for when the browser is too old
# or the latency is annoying).
#
# The far end of this is often a machine you would not choose: an old laptop,
# someone else's desktop, a phone. So the browser path is the one that is kept
# working, and the native path is the fallback rather than the other way round.
#
#   cloud-test.sh start      X + openbox + VNC + noVNC, then print the URLs
#   cloud-test.sh qemu       boot the NeoDCT image inside that X server
#   cloud-test.sh tunnel     (re)open the ngrok tunnels and print the URLs
#   cloud-test.sh shot FILE  grab the framebuffer to a PNG, for the agent
#   cloud-test.sh key K      send one key to the QEMU window (see KEYS below)
#   cloud-test.sh status     what is running, and where
#   cloud-test.sh stop       tear it all down
#
# ============ THE TUNNEL IS THE PART THAT DOES NOT ALWAYS WORK ============
#
# Everything above `tunnel` works in any container. `tunnel` depends on what
# the host lets out, and the answer is often "not this".
#
# A Claude Code cloud session sends ALL egress through a policy proxy
# (HTTPS_PROXY, see /root/.ccr/README.md). The ngrok agent detects that and
# refuses on the free plan:
#
#     authentication failed: Running the agent with an http/s proxy is a
#     Pay-as-you-go feature.                              ERR_NGROK_9009
#
# That is enforced at ngrok's end, so no amount of local configuration fixes
# it. The three ways out, in the order worth trying:
#
#   1. Tailscale. tailscaled in userspace mode honours HTTPS_PROXY and its
#      DERP relay is ordinary HTTPS, so it is the one most likely to survive
#      a policy proxy. Connect to the tailnet address on VNC_PORT; no inbound
#      port and no public URL at all, which is also the safest of the three.
#   2. An ngrok Pay-as-you-go plan, after which `proxy_url` is permitted and
#      this script works unchanged.
#   3. cloudflared, whose edge transport may or may not proxy.
#
# DO NOT "fix" this by unsetting HTTPS_PROXY. It is the host's egress policy,
# not a misconfiguration, and working around it is out of bounds even when
# the person asking owns the machine.
#
# ============ THE CONFIG FILE HOLDS A SECRET ============
#
# `tunnel` reads $RUNDIR/ngrok.yml, which this script deliberately does NOT
# create: it carries an account authtoken and RUNDIR is under /tmp so that it
# cannot be committed by accident. Write it by hand:
#
#     mkdir -p /tmp/neodct-cloud && chmod 700 /tmp/neodct-cloud
#     cat > /tmp/neodct-cloud/ngrok.yml <<'YML'
#     version: "3"
#     agent:
#       authtoken: <yours>
#     endpoints:
#       - name: vnc            # a native viewer: TigerVNC, RealVNC, Remmina
#         url: tcp://
#         upstream: { url: 5901 }
#       - name: novnc          # a browser, which is what an old machine has
#         url: https://
#         upstream: { url: 6080 }
#     YML
#     chmod 600 /tmp/neodct-cloud/ngrok.yml
#
# ============ WHY A REAL X SERVER AND NOT QEMU'S -vnc ============
#
# qemu -vnc is one window with no desktop: no way to resize it, no way to open
# a second thing beside it, and nothing on screen when QEMU exits. Xtigervnc
# plus a window manager costs about 40 MB and gives a session that survives
# QEMU restarting, which is most of what testing an OS looks like.
#
# ============ KEYS ============
#
# The phone's keypad is sixteen keys and QEMU delivers them as an ordinary
# keyboard. `key` takes xdotool names: Return, BackSpace, Up, Down, 1..9, 0,
# and for the two abused codes, shift (= '*') and backslash (= '#') -- see
# nd_keycodes.h for why those two are what they are.

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

VNC_DISPLAY="${NEODCT_VNC_DISPLAY:-:1}"
VNC_PORT=$((5900 + ${VNC_DISPLAY#:}))
NOVNC_PORT="${NEODCT_NOVNC_PORT:-6080}"
GEOMETRY="${NEODCT_VNC_GEOMETRY:-1024x768}"
RUNDIR="${NEODCT_CLOUD_RUNDIR:-/tmp/neodct-cloud}"
PASSFILE="$RUNDIR/vncpasswd"
PLAINFILE="$RUNDIR/password.txt"

mkdir -p "$RUNDIR"

log() { printf '[cloud-test] %s\n' "$*"; }
die() { printf '[cloud-test] %s\n' "$*" >&2; exit 1; }

# ============ WHY THESE ARE PORT CHECKS AND NOT pgrep ============
#
# `pgrep -f qemu-system-aarch64` matches THE SHELL THAT INVOKED THIS SCRIPT,
# because that shell's command line contains the string it is searching for.
# So status cheerfully reported qemu and ngrok as running when neither was.
# `pgrep -x` does not save you either: Linux truncates a process name to 15
# characters, so "qemu-system-aarch64" can never match exactly.
#
# A listening socket is the honest question anyway -- "can something connect
# to it" rather than "does a process with that name exist" -- so the network
# services are checked that way, and the two that are not servers get a
# pidfile and a short-enough name respectively.
listening() { ss -ltn 2>/dev/null | awk -v p=":$1" '$4 ~ p"$" {found=1} END {exit !found}'; }
qemu_pid()  { [ -f "$RUNDIR/qemu.pid" ] && kill -0 "$(cat "$RUNDIR/qemu.pid")" 2>/dev/null; }

# ------------------------------------------------------------------ #
# start
# ------------------------------------------------------------------ #

cmd_start() {
    if [ -e "/tmp/.X11-unix/X${VNC_DISPLAY#:}" ]; then
        log "display $VNC_DISPLAY is already up"
    else
        # A password even though the tunnel is the only way in: ngrok URLs are
        # guessable enough to be worth one, and a VNC session with no password
        # is a shell on this container to anyone who finds it.
        if [ ! -f "$PASSFILE" ]; then
            pw="$(tr -dc 'a-z0-9' < /dev/urandom | head -c 8)"
            printf '%s\n' "$pw" > "$PLAINFILE"
            chmod 600 "$PLAINFILE"
            printf '%s\n%s\n\n' "$pw" "$pw" | vncpasswd -f > "$PASSFILE" 2>/dev/null \
                || { printf '%s\n' "$pw" | vncpasswd -f > "$PASSFILE"; }
            chmod 600 "$PASSFILE"
        fi

        log "starting Xtigervnc on $VNC_DISPLAY ($GEOMETRY)"
        Xtigervnc "$VNC_DISPLAY" \
            -geometry "$GEOMETRY" -depth 24 \
            -rfbport "$VNC_PORT" -rfbauth "$PASSFILE" \
            -localhost=0 -AlwaysShared -SecurityTypes VncAuth \
            -desktop "NeoDCT cloud test" \
            > "$RUNDIR/xvnc.log" 2>&1 &
        # The X socket appears a beat after the process does; everything below
        # fails confusingly without it.
        i=0
        while [ ! -e "/tmp/.X11-unix/X${VNC_DISPLAY#:}" ] && [ $i -lt 50 ]; do
            i=$((i + 1)); sleep 0.2
        done
        [ -e "/tmp/.X11-unix/X${VNC_DISPLAY#:}" ] || die "Xtigervnc did not come up; see $RUNDIR/xvnc.log"
    fi

    if ! pgrep -f "openbox.*$VNC_DISPLAY" >/dev/null 2>&1 && ! pgrep -x openbox >/dev/null 2>&1; then
        log "starting openbox"
        DISPLAY="$VNC_DISPLAY" openbox > "$RUNDIR/openbox.log" 2>&1 &
        sleep 1
    fi
    # A mid-grey root window rather than the default stipple: a 240x175 phone
    # panel on a black desktop is hard to find, and the stipple moires.
    DISPLAY="$VNC_DISPLAY" xsetroot -solid '#20242c' 2>/dev/null || true

    if ! listening "$NOVNC_PORT"; then
        log "starting noVNC on :$NOVNC_PORT"
        websockify --web=/usr/share/novnc "$NOVNC_PORT" "localhost:$VNC_PORT" \
            > "$RUNDIR/novnc.log" 2>&1 &
        sleep 1
    fi

    log "up. VNC password: $(cat "$PLAINFILE" 2>/dev/null || echo '(see '"$PASSFILE"')')"
}

# ------------------------------------------------------------------ #
# qemu
# ------------------------------------------------------------------ #

cmd_qemu() {
    [ -e "/tmp/.X11-unix/X${VNC_DISPLAY#:}" ] || die "run 'cloud-test.sh start' first"
    if qemu_pid; then
        log "qemu is already running as pid $(cat "$RUNDIR/qemu.pid")"
        return 0
    fi
    log "booting the image on $VNC_DISPLAY"
    # run_qemu.sh's own defaults, with the display it already knows how to
    # take. Everything else -- snapshot, verity, the card -- stays the
    # caller's to set through its documented environment variables.
    DISPLAY="$VNC_DISPLAY" setsid nohup "$HERE/run_qemu.sh" > "$RUNDIR/qemu.log" 2>&1 &
    echo $! > "$RUNDIR/qemu.pid"
    sleep 3
    tail -5 "$RUNDIR/qemu.log" 2>/dev/null || true
}

# ------------------------------------------------------------------ #
# tunnel
# ------------------------------------------------------------------ #

cmd_tunnel() {
    command -v ngrok >/dev/null 2>&1 || die "ngrok is not installed"
    pkill -x ngrok 2>/dev/null || true
    sleep 1

    log "opening tunnels"
    nohup ngrok start --all --config "$RUNDIR/ngrok.yml" > "$RUNDIR/ngrok.log" 2>&1 &
    # ngrok's local API is the only reliable way to learn the public URL.
    i=0
    while [ $i -lt 40 ]; do
        if curl -s --noproxy '*' http://127.0.0.1:4040/api/tunnels 2>/dev/null | grep -q public_url; then
            break
        fi
        i=$((i + 1)); sleep 0.5
    done
    curl -s --noproxy '*' http://127.0.0.1:4040/api/tunnels 2>/dev/null \
        | python3 -c 'import sys,json
d=json.load(sys.stdin)
for t in d.get("tunnels",[]):
    print("  %-8s %s" % (t["name"], t["public_url"]))' 2>/dev/null \
        || { log "no tunnels yet; see $RUNDIR/ngrok.log"; return 1; }
}

# ------------------------------------------------------------------ #
# shot / key -- what the AGENT uses, rather than a human at the far end
# ------------------------------------------------------------------ #

cmd_shot() {
    out="${1:-$RUNDIR/screen.png}"
    [ -e "/tmp/.X11-unix/X${VNC_DISPLAY#:}" ] || die "no display"
    DISPLAY="$VNC_DISPLAY" import -window root "$out" 2>/dev/null \
        || DISPLAY="$VNC_DISPLAY" xwd -root -silent 2>/dev/null | convert xwd:- "$out" 2>/dev/null \
        || die "no screenshot tool (install imagemagick)"
    log "wrote $out"
}

cmd_key() {
    [ $# -ge 1 ] || die "usage: cloud-test.sh key <name>"
    command -v xdotool >/dev/null 2>&1 || die "xdotool is not installed"
    win="$(DISPLAY="$VNC_DISPLAY" xdotool search --name 'QEMU' | head -1)"
    [ -n "$win" ] || die "no QEMU window on $VNC_DISPLAY"
    DISPLAY="$VNC_DISPLAY" xdotool key --window "$win" "$@"
}

# ------------------------------------------------------------------ #
# status / stop
# ------------------------------------------------------------------ #

cmd_status() {
    printf 'display   %s  %s\n' "$VNC_DISPLAY" \
        "$([ -e "/tmp/.X11-unix/X${VNC_DISPLAY#:}" ] && echo up || echo down)"
    printf 'vnc       :%s  %s\n' "$VNC_PORT" \
        "$(listening "$VNC_PORT" && echo up || echo down)"
    printf 'openbox        %s\n' "$(pgrep -x openbox >/dev/null && echo up || echo down)"
    printf 'novnc     :%s  %s\n' "$NOVNC_PORT" \
        "$(listening "$NOVNC_PORT" && echo up || echo down)"
    printf 'qemu           %s\n' "$(qemu_pid && echo up || echo down)"
    # -x, not -f: `pgrep -f 'ngrok '` also matches the shell that invoked this
    # script, so status cheerfully reported a tunnel that had already died.
    printf 'ngrok          %s\n' "$(pgrep -x ngrok >/dev/null && echo up || echo down)"
    if pgrep -x ngrok >/dev/null 2>&1; then
        curl -s --noproxy '*' http://127.0.0.1:4040/api/tunnels 2>/dev/null \
            | python3 -c 'import sys,json
d=json.load(sys.stdin)
for t in d.get("tunnels",[]):
    print("  %-8s %s" % (t["name"], t["public_url"]))' 2>/dev/null || true
    fi
    [ -f "$PLAINFILE" ] && printf 'password  %s\n' "$(cat "$PLAINFILE")"
    return 0
}

cmd_stop() {
    pkill -x ngrok 2>/dev/null || true
    if qemu_pid; then kill "$(cat "$RUNDIR/qemu.pid")" 2>/dev/null || true; fi
    rm -f "$RUNDIR/qemu.pid"
    pkill -f websockify 2>/dev/null || true
    pkill -x openbox 2>/dev/null || true
    pkill -f Xtigervnc 2>/dev/null || true
    log "stopped"
}

case "${1:-status}" in
    start)  cmd_start ;;
    qemu)   cmd_qemu ;;
    tunnel) cmd_tunnel ;;
    shot)   shift; cmd_shot "$@" ;;
    key)    shift; cmd_key "$@" ;;
    status) cmd_status ;;
    stop)   cmd_stop ;;
    *)      die "usage: cloud-test.sh {start|qemu|tunnel|shot|key|status|stop}" ;;
esac
