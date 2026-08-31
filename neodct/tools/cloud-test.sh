#!/bin/sh
# cloud-test.sh -- put the phone's screen on the internet.
#
# A cloud agent can build this OS and run its tests, but it cannot LOOK at it.
# This closes that gap: an X server with no monitor, a window manager, the
# QEMU window inside it, and two ways in from outside -- a browser (noVNC over
# a wstunnel reverse tunnel to a VPS you own, which needs nothing installed at
# the far end) and a native VNC client on the same session.
#
# The far end of this is often a machine you would not choose: an old laptop,
# someone else's desktop, a phone. So the browser path is the one that is kept
# working, and the native path is the fallback rather than the other way round.
#
#   cloud-test.sh start      X + openbox + VNC + noVNC, then print the URLs
#   cloud-test.sh qemu       boot the NeoDCT image inside that X server
#   cloud-test.sh tunnel     reverse-tunnel noVNC to your VPS over wss/443
#   cloud-test.sh vps        print what to run on the VPS to receive it
#   cloud-test.sh shot FILE  grab the whole desktop to a PNG, for the agent
#   cloud-test.sh panel FILE grab JUST the phone panel, which is what to look at
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
# it.
#
# ============ WHAT THE NETWORK ACTUALLY ALLOWS, MEASURED ============
#
# cloudflared's preflight is the quickest way to find out, and on a Claude
# Code cloud session it reports:
#
#     DNS Resolution    region1.v2.argotunnel.com   PASS
#     UDP Connectivity  region1.v2.argotunnel.com   FAIL  (QUIC)
#     TCP Connectivity  region1.v2.argotunnel.com   FAIL  (port 7844)
#     Cloudflare API    api.cloudflare.com:443      PASS
#
# So: DNS works, HTTPS on 443 THROUGH THE PROXY works, and nothing else gets
# out at all. That rules cloudflared out completely -- its edge connection is
# a raw dial to port 7844 and every --proxy-* flag it has is about the ORIGIN
# side, not the edge, so there is no way to put it through the proxy.
#
# ============ THE PROXY IS A TLS-ONLY MITM, AND THAT DECIDES EVERYTHING ====
#
# It is not a transparent CONNECT tunnel. /root/.ccr/README.md says "TLS is
# re-terminated there", and the consequences are bigger than they sound. Four
# things were measured against a VPS running wstunnel, watching BOTH ends:
#
#   1. "200 Connection Established" IS FABRICATED. The proxy returns it for
#      any host:port, including ports with nothing behind them --
#      67.205.190.49:9999 answers 200 exactly like :443 does. It proves
#      nothing, and it is the single most misleading output in this whole
#      exercise. Do not conclude reachability from it.
#
#   2. NON-TLS BYTES GET NOTHING. CONNECT to a host's port 22 and sshd's
#      banner -- which a real sshd sends the instant it accepts -- never
#      arrives. The proxy is waiting for a ClientHello and will not carry a
#      protocol that does not start with one.
#
#   3. ON 443 IT VALIDATES THE ORIGIN CERTIFICATE. wstunnel's auto-generated
#      self-signed cert was rejected: the server logged connections arriving
#      and then erroring, while the client saw a reset. Turning off
#      verification CLIENT-side does not help -- it is the proxy doing the
#      validating, and it is not ours to configure.
#
#   4. OFF 443 IT DOES NOT REALLY CONNECT. Same server, same self-signed
#      cert, on 8080: the server logged NOTHING at all. And plain ws:// on
#      8080 arrived as a forward-proxied GET with the WebSocket upgrade
#      headers stripped, which the server correctly answered 400:
#
#          Invalid protocol version request, got HTTP/1.1 while expecting
#          either websocket http1 upgrade or http2
#
# So exactly one shape of traffic crosses this proxy: TLS, on port 443, to a
# host presenting a PUBLICLY TRUSTED certificate, addressed BY HOSTNAME (the
# proxy needs a name to validate against, and TLS needs one for SNI).
#
# ============ WHAT THAT MEANS FOR EACH TUNNEL ============
#
#   cloudflared   Dead. Its edge is a raw dial to port 7844 and every
#                 --proxy-* flag it has is origin-side. No way in.
#
#   ngrok         DOUBTFUL, AND DO NOT PAY TO FIND OUT. The plan gate is
#                 real, but clearing it is not obviously enough: the agent
#                 speaks TLS to connect.ngrok-agent.com, and an agent
#                 protocol that validates or pins its server certificate
#                 breaks against a MITM exactly as our self-signed cert did.
#                 UNTESTED -- nobody has run a paid agent from here. An
#                 earlier draft of this file said the path was open and the
#                 plan was the only obstacle. That was written before the
#                 MITM was understood and it should not be trusted.
#
#   Tailscale     Same doubt, same reason. controlplane.tailscale.com and
#                 derp1.tailscale.com both answer through the proxy, which
#                 is necessary but not sufficient; whether tailscaled's own
#                 certificate checking survives interception is UNTESTED.
#
#   wstunnel      WORKS, because it is the only one where you own both ends.
#                 The origin needs a real certificate -- see below -- and
#                 after that it is ordinary HTTPS as far as the proxy can
#                 tell.
#
# ============ THE wstunnel RECIPE, WHICH IS THE ONE THAT WORKS ============
#
# On a VPS, with sslip.io supplying a free hostname for a bare IP so that no
# domain has to be bought:
#
#     sudo certbot certonly --standalone -d <a-b-c-d>.sslip.io
#     sudo wstunnel server \
#       --restrict-http-upgrade-path-prefix "$SECRET" \
#       --tls-certificate /etc/letsencrypt/live/<a-b-c-d>.sslip.io/fullchain.pem \
#       --tls-private-key /etc/letsencrypt/live/<a-b-c-d>.sslip.io/privkey.pem \
#       wss://[::]:443
#
# and in the container, where --http-proxy is the flag the other two lacked:
#
#     wstunnel client -R 'tcp://[::]:6080:127.0.0.1:6080' \
#       --http-upgrade-path-prefix "$SECRET" \
#       --http-proxy "$HTTPS_PROXY" \
#       wss://<a-b-c-d>.sslip.io:443
#
# Then the far end opens http://<vps>:6080/vnc.html in any browser. Watch for
# SNI DnsName(...) rather than SNI IpAddress(...) in the client log: the
# hostname is what lets the proxy's validation succeed, and connecting to the
# bare IP fails even with a valid certificate on the far end.
#
# DO NOT "fix" this by unsetting HTTPS_PROXY. It is the host's egress policy,
# not a misconfiguration, and working around it is out of bounds even when
# the person asking owns the machine.
#
# ============ THE TUNNEL HOLDS A SECRET ============
#
# `tunnel` needs two things and keeps both in $RUNDIR, under /tmp, so that
# neither can be committed by accident:
#
#   wstunnel.secret   the --http-upgrade-path-prefix. The SERVER restricts on
#                     the same string, so it is the only thing between a
#                     passer-by who finds the port and this container's
#                     screen. Generated on first use.
#   wstunnel.host     the VPS, remembered so it is typed once.
#
#     NEODCT_TUNNEL_HOST=<a-b-c-d>.sslip.io cloud-test.sh tunnel
#     cloud-test.sh vps      # prints what to run on the far end
#
# The host must be a NAME. See the proxy notes above: the proxy validates the
# origin certificate itself and does it by name, so an address fails even
# with a valid certificate. sslip.io turns an address into a name for free
# and `tunnel` does that conversion rather than let it fail obscurely.
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
# The wstunnel upgrade-path prefix. Both ends must carry the same string,
# and it stays in $RUNDIR rather than the repo -- it is the only thing
# standing between a passer-by and this container's screen.
SECRETFILE="$RUNDIR/wstunnel.secret"

mkdir -p "$RUNDIR"

log() { printf '[cloud-test] %s\n' "$*"; }
die() { printf '[cloud-test] %s\n' "$*" >&2; exit 1; }

# ============ EVERYTHING HERE OUTLIVES THE SHELL THAT STARTS IT ============
#
# A plain `cmd &` is a child of this script, and this script is usually a child
# of an agent's tool call. When that call returns, the process group is reaped
# and the whole session dies -- silently, minutes later, so the first symptom
# is a VNC client that cannot connect to a display that was definitely up.
#
# It cost this file three separate outages (the X server, the terminals, and a
# four-hour Buildroot run that stopped mid-line with no error) before the
# pattern was recognised. setsid detaches into a new session so nothing
# upstream can take it with them.
spawn() {
    log_file="$1"; shift
    setsid nohup "$@" </dev/null >>"$log_file" 2>&1 &
}

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
# The desktop openbox needs to be usable
# ------------------------------------------------------------------ #

# Openbox ships no root menu worth having, and on a screen full of windows
# there is frequently no desktop left to right-click on anyway -- which reads
# exactly like a broken window manager. So: a menu with the things this session
# actually needs, and a keybinding that works wherever the pointer is.
install_desktop_config() {
    conf="${HOME:-/root}/.config/openbox"
    mkdir -p "$conf"

    cat > "$conf/menu.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
<menu id="root-menu" label="NeoDCT">
  <item label="Terminal">
    <action name="Execute"><execute>xterm -fa Monospace -fs 11 -bg #0b0e13 -fg #c8d0e0 -geometry 100x30</execute></action>
  </item>
  <separator label="Phone"/>
  <item label="Boot NeoDCT (QEMU)">
    <action name="Execute"><execute>CLOUD_TEST_SH qemu</execute></action>
  </item>
  <item label="Stop QEMU">
    <action name="Execute"><execute>CLOUD_TEST_SH stop-qemu</execute></action>
  </item>
  <item label="QEMU log">
    <action name="Execute"><execute>xterm -T "qemu log" -fa Monospace -fs 10 -geometry 110x30 -e sh -c "tail -f RUNDIR_PATH/qemu.log"</execute></action>
  </item>
  <separator/>
  <item label="Reconfigure Openbox"><action name="Reconfigure"/></item>
</menu>
</openbox_menu>
XML
    sed -i "s#CLOUD_TEST_SH#$HERE/cloud-test.sh#g; s#RUNDIR_PATH#$RUNDIR#g" "$conf/menu.xml"

    # Ctrl+Alt+M for the menu, Ctrl+Alt+T for a terminal. Both matter because
    # a maximised window leaves nowhere to right-click.
    if [ ! -f "$conf/rc.xml" ] && [ -f /etc/xdg/openbox/rc.xml ]; then
        sed 's#<keyboard>#<keyboard>\
    <keybind key="C-A-m"><action name="ShowMenu"><menu>root-menu</menu></action></keybind>\
    <keybind key="C-A-t"><action name="Execute"><execute>xterm -fa Monospace -fs 11 -geometry 100x28</execute></action></keybind>#' \
            /etc/xdg/openbox/rc.xml > "$conf/rc.xml"
    fi
}

# ------------------------------------------------------------------ #
# Is the display really there?
# ------------------------------------------------------------------ #
#
# NOT `[ -e /tmp/.X11-unix/X1 ]`, which is what every check in here used to
# be. That socket is an ordinary file in an ordinary directory: it is created
# by the X server and removed by the X server, and a server that was killed
# rather than asked to stop -- a container restarted, an OOM, a `docker stop`
# -- leaves it behind. /tmp/.X1-lock outlives it the same way.
#
# The symptom is the worst kind. `start` says "display :1 is already up" and
# does nothing, then openbox says "Failed to open the display from the
# DISPLAY environment variable", then qemu says "gtk initialization failed",
# and `status` cheerfully reports the display as up throughout. Three
# unrelated-looking errors, none of them naming the cause, and the fix
# (delete a file) is not one anybody guesses.
#
# So: ask the server, do not look for its socket. xdpyinfo opens a real
# connection and fails if nothing answers.
display_alive() {
    xdpyinfo -display "$VNC_DISPLAY" >/dev/null 2>&1
}

# A socket with no server behind it. Xtigervnc refuses to start while either
# it or the lock file is there, so a stale pair has to be cleared rather than
# ignored.
clear_stale_display() {
    log "display $VNC_DISPLAY has a socket but nothing behind it -- clearing"
    rm -f "/tmp/.X11-unix/X${VNC_DISPLAY#:}" "/tmp/.X${VNC_DISPLAY#:}-lock"
}

# ------------------------------------------------------------------ #
# start
# ------------------------------------------------------------------ #

cmd_start() {
    if display_alive; then
        log "display $VNC_DISPLAY is already up"
    else
        [ -e "/tmp/.X11-unix/X${VNC_DISPLAY#:}" ] && clear_stale_display
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
        spawn "$RUNDIR/xvnc.log" Xtigervnc "$VNC_DISPLAY" \
            -geometry "$GEOMETRY" -depth 24 \
            -rfbport "$VNC_PORT" -rfbauth "$PASSFILE" \
            -localhost=0 -AlwaysShared -SecurityTypes VncAuth \
            -desktop "NeoDCT cloud test"
        # The X socket appears a beat after the process does; everything below
        # fails confusingly without it.
        i=0
        while ! display_alive && [ $i -lt 50 ]; do
            i=$((i + 1)); sleep 0.2
        done
        display_alive || die "Xtigervnc did not come up; see $RUNDIR/xvnc.log"
    fi

    if ! pgrep -x openbox >/dev/null 2>&1; then
        log "starting openbox"
        # openbox has NO --display flag. It reads DISPLAY from the environment
        # and exits instantly with "Invalid command line argument" otherwise,
        # which leaves a session with no window manager -- no title bars and,
        # more confusingly, a desktop where right-click does nothing.
        install_desktop_config
        spawn "$RUNDIR/openbox.log" env DISPLAY="$VNC_DISPLAY" openbox
        sleep 2
    fi
    # A mid-grey root window rather than the default stipple: a 240x175 phone
    # panel on a black desktop is hard to find, and the stipple moires.
    DISPLAY="$VNC_DISPLAY" xsetroot -solid '#20242c' 2>/dev/null || true

    if ! listening "$NOVNC_PORT"; then
        log "starting noVNC on :$NOVNC_PORT"
        spawn "$RUNDIR/novnc.log" websockify --web=/usr/share/novnc \
            "$NOVNC_PORT" "localhost:$VNC_PORT"
        sleep 2
    fi

    log "up. VNC password: $(cat "$PLAINFILE" 2>/dev/null || echo '(see '"$PASSFILE"')')"
}

# ------------------------------------------------------------------ #
# qemu
# ------------------------------------------------------------------ #

cmd_qemu() {
    display_alive || die "run 'cloud-test.sh start' first"
    if qemu_pid; then
        log "qemu is already running as pid $(cat "$RUNDIR/qemu.pid")"
        return 0
    fi
    log "booting the image on $VNC_DISPLAY"
    # XDG_RUNTIME_DIR is unset in a container with no login session, and QEMU's
    # GTK frontend wants one. That IS specific to running it this way, so it
    # belongs here.
    #
    # The audio backend used to be forced to none here too, with a comment
    # claiming it was not run_qemu.sh's concern. That was wrong: run_qemu.sh
    # defaulted to PulseAudio, so anyone who ran it directly on a headless box
    # -- which is most of the machines this rig exists to serve -- got two
    # fatal errors that never mention audio. It probes for a sound server
    # itself now, and this script deliberately does not second-guess it: one
    # default in one place is what stops the two drifting apart again, which
    # is exactly how the bug got there.
    if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
        XDG_RUNTIME_DIR="$RUNDIR/xdg"
        mkdir -p "$XDG_RUNTIME_DIR"
        chmod 700 "$XDG_RUNTIME_DIR"
        export XDG_RUNTIME_DIR
    fi
    # run_qemu.sh's own defaults, with the display it already knows how to
    # take. Everything else -- snapshot, verity, the card -- stays the
    # caller's to set through its documented environment variables.
    spawn "$RUNDIR/qemu.log" env DISPLAY="$VNC_DISPLAY" "$HERE/run_qemu.sh"
    echo $! > "$RUNDIR/qemu.pid"
    sleep 3
    tail -5 "$RUNDIR/qemu.log" 2>/dev/null || true
}

# ------------------------------------------------------------------ #
# tunnel
# ------------------------------------------------------------------ #

cmd_tunnel() {
    # wstunnel, NOT ngrok. This used to run ngrok and that was wrong in a way
    # that cost a whole session: the measurements are in
    # .claude/skills/neodct-app/references/cloud-testing.md and they say ngrok
    # cannot work here. Its free plan refuses to run behind a proxy at all
    # (ERR_NGROK_9009), and paying would not obviously help, because its agent
    # speaks TLS to its own edge and would break against this proxy's MITM the
    # same way a self-signed certificate does.
    #
    # The container's egress is one shape of traffic and one only: TLS, on
    # port 443, to a host with a publicly trusted certificate, ADDRESSED BY
    # HOSTNAME. wstunnel is the only tunnel of the three that fits, because
    # you own both ends -- so the origin can have a real certificate -- and
    # because it is the only one with an --http-proxy flag.
    #
    #     NEODCT_TUNNEL_HOST=1-2-3-4.sslip.io cloud-test.sh tunnel
    #
    # The host is remembered in $RUNDIR/wstunnel.host, so it is given once.
    command -v wstunnel >/dev/null 2>&1 || die "wstunnel is not installed
  curl -L -o /tmp/w.tgz https://github.com/erebe/wstunnel/releases/download/v10.7.0/wstunnel_10.7.0_linux_amd64.tar.gz
  tar -xzf /tmp/w.tgz -C /usr/local/bin wstunnel"

    hostfile="$RUNDIR/wstunnel.host"
    host="${NEODCT_TUNNEL_HOST:-$(cat "$hostfile" 2>/dev/null || true)}"
    [ -n "$host" ] || die "no tunnel host.
  Set one: NEODCT_TUNNEL_HOST=<a-b-c-d>.sslip.io cloud-test.sh tunnel
  See 'cloud-test.sh vps' for what to run on the far end."

    # An IP is the documented way to get this wrong. The proxy validates the
    # origin certificate itself and it does that by NAME: connecting by
    # address fails even when the certificate is valid, and the log line that
    # tells you is SNI IpAddress(...) where it should read SNI DnsName(...).
    # sslip.io resolves a-b-c-d.sslip.io to a.b.c.d, so an address can always
    # be turned into a name without buying a domain -- do it here rather than
    # let it fail in a way that looks like a server problem.
    case "$host" in
        *[0-9].[0-9]*.[0-9]*.[0-9]*)
            case "$host" in
                *.sslip.io|*.nip.io) : ;;
                *[a-zA-Z]*) : ;;
                *)
                    dashed=$(printf '%s' "$host" | tr '.' '-')
                    log "$host is an address; using $dashed.sslip.io so the proxy can validate by name"
                    host="$dashed.sslip.io"
                    ;;
            esac
            ;;
    esac
    printf '%s\n' "$host" > "$hostfile"

    if [ ! -f "$SECRETFILE" ]; then
        tr -dc 'a-zA-Z0-9' < /dev/urandom | head -c 48 > "$SECRETFILE"
        chmod 600 "$SECRETFILE"
        log "generated a new upgrade-path secret; the server needs the same one"
    fi
    secret="$(cat "$SECRETFILE")"

    pkill -f 'wstunnel client' 2>/dev/null || true
    sleep 1
    log "tunnelling :$NOVNC_PORT to $host over wss/443"
    # setsid: a plain & dies with the shell this tool call runs in, and the
    # tunnel is meant to outlive it.
    setsid env RUST_LOG=info wstunnel client \
        -R "tcp://[::]:${NOVNC_PORT}:127.0.0.1:${NOVNC_PORT}" \
        --http-upgrade-path-prefix "$secret" \
        --http-proxy "${HTTPS_PROXY:-}" \
        "wss://${host}:443" >> "$RUNDIR/wstunnel.log" 2>&1 &
    sleep 4

    if grep -q "SNI IpAddress" "$RUNDIR/wstunnel.log" 2>/dev/null; then
        log "WARNING: connecting by address, not name -- the proxy will reject this"
    fi
    if pgrep -f 'wstunnel client' >/dev/null 2>&1; then
        log "up:  http://${host%%.sslip.io}:$NOVNC_PORT/vnc.html   (or http://$host:$NOVNC_PORT/vnc.html)"
        log "vnc password: $(cat "$PLAINFILE" 2>/dev/null || echo '(see '"$PASSFILE"')')"
        log "it retries on its own, so starting the server end later is fine"
    else
        log "wstunnel exited; see $RUNDIR/wstunnel.log"
        return 1
    fi
}

# What to run on the far end. Printed rather than done, because it is somebody
# else's machine and needs root there.
cmd_vps() {
    hostfile="$RUNDIR/wstunnel.host"
    host="${NEODCT_TUNNEL_HOST:-$(cat "$hostfile" 2>/dev/null || echo '<a-b-c-d>.sslip.io')}"
    secret="$(cat "$SECRETFILE" 2>/dev/null || echo '<run tunnel once to generate one>')"
    cat <<VPSEOF
On the VPS, once:

    sudo certbot certonly --standalone -d $host

Then, to serve (the path prefix must match this container's secret):

    sudo wstunnel server \\
      --restrict-http-upgrade-path-prefix '$secret' \\
      --tls-certificate /etc/letsencrypt/live/$host/fullchain.pem \\
      --tls-private-key  /etc/letsencrypt/live/$host/privkey.pem \\
      'wss://[::]:443'

Then open, from anything -- a phone, a cheap laptop:

    http://$host:$NOVNC_PORT/vnc.html

A reset from this container means the far end is not answering on 443: either
wstunnel server is not running or its certificate is not one a public trust
store accepts. The proxy fabricates "200 Connection Established" for any
host:port, so a successful CONNECT proves nothing.
VPSEOF
}

# ------------------------------------------------------------------ #
# shot / key -- what the AGENT uses, rather than a human at the far end
# ------------------------------------------------------------------ #

cmd_shot() {
    out="${1:-$RUNDIR/screen.png}"
    display_alive || die "no display"
    DISPLAY="$VNC_DISPLAY" import -window root "$out" 2>/dev/null \
        || DISPLAY="$VNC_DISPLAY" xwd -root -silent 2>/dev/null | convert xwd:- "$out" 2>/dev/null \
        || die "no screenshot tool (install imagemagick)"
    log "wrote $out"
}

# ============ WHY THE QEMU WINDOW IS PICKED BY SIZE ============
#
# QEMU owns TWO windows whose names match "QEMU": the visible one, and a 10x10
# icon window named "qemu" in lower case. xdotool's --name is a case-INSENSITIVE
# regex, so `search --name QEMU | head -1` returns whichever the server lists
# first, and that is usually the icon. Keys sent there go nowhere at all, which
# looks exactly like a hung phone.
#
# Geometry is the honest discriminator: the panel window is 320x240 and the
# icon is 10x10.
qemu_window() {
    DISPLAY="$VNC_DISPLAY" xdotool search --name QEMU 2>/dev/null | while read -r w; do
        wide=$(DISPLAY="$VNC_DISPLAY" xdotool getwindowgeometry "$w" 2>/dev/null |
                   sed -n 's/.*Geometry: \([0-9]*\)x[0-9]*.*/\1/p')
        [ -n "$wide" ] && [ "$wide" -gt 100 ] && echo "$w"
    done | tail -1
}

cmd_key() {
    [ $# -ge 1 ] || die "usage: cloud-test.sh key <name>"
    command -v xdotool >/dev/null 2>&1 || die "xdotool is not installed"
    win="$(qemu_window)"
    [ -n "$win" ] || die "no QEMU window on $VNC_DISPLAY"
    # Activate first, then send through XTEST rather than with --window: SDL
    # ignores the synthetic events XSendEvent delivers, so --window presses are
    # accepted by the X server and dropped by QEMU.
    DISPLAY="$VNC_DISPLAY" xdotool windowactivate --sync "$win" 2>/dev/null || true
    for k in "$@"; do
        DISPLAY="$VNC_DISPLAY" xdotool key --clearmodifiers "$k"
        sleep 0.4
    done
}

# The panel alone, without the desktop around it -- what an agent actually
# wants to look at. 240x175 upscaled is unreadable to a vision model otherwise.
cmd_panel() {
    out="${1:-$RUNDIR/panel.png}"
    win="$(qemu_window)"
    [ -n "$win" ] || die "no QEMU window on $VNC_DISPLAY"
    DISPLAY="$VNC_DISPLAY" import -window "$win" "$out" 2>/dev/null \
        || die "could not capture the QEMU window"
    log "wrote $out"
}

# ------------------------------------------------------------------ #
# status / stop
# ------------------------------------------------------------------ #

cmd_status() {
    printf 'display   %s  %s\n' "$VNC_DISPLAY" \
        "$(display_alive && echo up || echo down)"
    printf 'vnc       :%s  %s\n' "$VNC_PORT" \
        "$(listening "$VNC_PORT" && echo up || echo down)"
    printf 'openbox        %s\n' "$(pgrep -x openbox >/dev/null && echo up || echo down)"
    printf 'novnc     :%s  %s\n' "$NOVNC_PORT" \
        "$(listening "$NOVNC_PORT" && echo up || echo down)"
    printf 'qemu           %s\n' "$(qemu_pid && echo up || echo down)"
    # A running client is not a connected one: it retries for as long as the
    # far end is silent, which is the normal state while somebody is still
    # starting the server. So report both, and separately.
    if pgrep -f 'wstunnel client' >/dev/null 2>&1; then
        host="$(cat "$RUNDIR/wstunnel.host" 2>/dev/null || echo '?')"
        if [ -f "$RUNDIR/wstunnel.log" ] && \
           tail -40 "$RUNDIR/wstunnel.log" 2>/dev/null | grep -q 'Tunnel created\|Starting TCP server'; then
            printf 'tunnel         up -> http://%s:%s/vnc.html\n' "$host" "$NOVNC_PORT"
        else
            printf 'tunnel         retrying -> %s (is wstunnel server up there?)\n' "$host"
        fi
    else
        printf 'tunnel         down\n'
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
    stop-qemu)
            if qemu_pid; then kill "$(cat "$RUNDIR/qemu.pid")" 2>/dev/null || true; fi
            rm -f "$RUNDIR/qemu.pid"; log "qemu stopped" ;;
    tunnel) cmd_tunnel ;;
    vps)    cmd_vps ;;
    shot)   shift; cmd_shot "$@" ;;
    key)    shift; cmd_key "$@" ;;
    panel)  shift; cmd_panel "$@" ;;
    status) cmd_status ;;
    stop)   cmd_stop ;;
    *)      die "usage: cloud-test.sh {start|qemu|stop-qemu|tunnel|vps|shot|panel|key|status|stop}" ;;
esac
