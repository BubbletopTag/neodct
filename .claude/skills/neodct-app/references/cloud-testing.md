# Running the phone from a cloud session

`neodct/tools/cloud-test.sh` puts an X server, a window manager, QEMU and VNC
in a headless container so an agent can boot the phone and look at it.

    cloud-test.sh start      X + openbox + VNC + noVNC
    cloud-test.sh qemu       boot the image inside that X server
    cloud-test.sh shot FILE  the framebuffer as a PNG   <- what the agent uses
    cloud-test.sh key NAME   one keypress into QEMU     <- and this
    cloud-test.sh status / stop / tunnel

`shot` and `key` are the half that needs no network at all, and on a locked
down host they are the whole product: boot, press, screenshot, look. That is a
testing loop an agent can drive alone and paste results from.

## Building the image first

`cloud-test.sh qemu` needs `buildroot/output/images/` to exist. A cold
Buildroot build is hours (cross toolchain, kernel, every package) and needs
`flex cpio rsync bc` beyond the usual. Start it and check on it:

    cd buildroot && make neodct_qemu_defconfig && make -j$(nproc)

**Start it with `setsid`.** A long job launched with a plain `&` from an agent
tool call gets reaped when its parent shell exits, and a Buildroot build dies
*silently* -- the log simply stops mid-line with no error and no
`make: *** Error`. If a build appears to have stalled, check `pgrep make` and
whether the log is still growing before looking for a compile failure. The same
applies to Xtigervnc, openbox, websockify and the QEMU process; anything meant
to outlive one tool call needs `setsid`.

Buildroot is incremental, so a killed build resumes where it stopped.

## Gotchas that cost real time

- **`openbox` has no `--display` flag.** It reads `DISPLAY` from the
  environment and exits instantly with
  `Invalid command line argument "--display"` otherwise. No WM means no root
  menu and a desktop where right-click does nothing.
- **Give openbox a menu and a keybinding.** The default root menu is empty. And
  on a screen full of windows there is often no desktop left to right-click, so
  bind `ShowMenu` to something like `C-A-m` in
  `~/.config/openbox/rc.xml`.
- **`pgrep -f qemu-system-aarch64` matches the shell that invoked your script**,
  because that shell's command line contains the string. `pgrep -x` cannot save
  you either: Linux truncates a process name to 15 characters and
  `qemu-system-aarch64` is 19. Check a listening socket or a pidfile instead.
- The panel is 240x175 and `run_qemu.sh` uses `zoom-to-fit=off`, so the QEMU
  window is small on a 1024x768 desktop. That is correct.

## Getting the screen out of the container -- read this before trying

A Claude Code cloud session routes **all** egress through a policy proxy
(`HTTPS_PROXY`, see `/root/.ccr/README.md`). It is not a transparent tunnel,
and four properties were measured the hard way:

1. **`200 Connection Established` is fabricated.** The proxy returns it for any
   host:port, including ports with nothing behind them. It proves nothing, and
   it is the single most misleading output in the exercise.
2. **Non-TLS bytes get nothing.** `CONNECT` to a host's port 22 and sshd's
   banner never arrives. The proxy waits for a TLS ClientHello and carries
   nothing that does not begin with one. This kills every "tunnel it inside a
   raw socket" idea, socat included.
3. **On 443 it validates the origin certificate.** A self-signed cert is
   rejected: the far server logs connections arriving and erroring while the
   client sees a reset. Client-side "skip verification" flags are irrelevant --
   the proxy is doing the validating and it is not yours to configure.
4. **Off 443 it does not really connect.** The same server on 8080 logs
   nothing at all, and plain `ws://` there arrives as a forward-proxied GET
   with the WebSocket upgrade headers stripped.

So exactly one shape of traffic crosses it: **TLS, on port 443, to a host with
a publicly trusted certificate, addressed by hostname.**

Consequences:

- **cloudflared**: dead. Its edge is a raw dial to port 7844 and every
  `--proxy-*` flag it has is origin-side.
- **ngrok**: the free plan refuses to run behind a proxy at all
  (`ERR_NGROK_9009`), and clearing that gate is not obviously enough -- its
  agent speaks TLS to `connect.ngrok-agent.com` and would break against a MITM
  the same way a self-signed cert does. Untested. Do not pay to find out.
- **wstunnel**: works, because you own both ends and can put a real certificate
  on the origin. It is also the only one of the three with an `--http-proxy`
  flag.

### The wstunnel recipe

On a VPS. `sslip.io` supplies a hostname for a bare IP, so no domain purchase:

    sudo certbot certonly --standalone -d <a-b-c-d>.sslip.io
    sudo wstunnel server \
      --restrict-http-upgrade-path-prefix "$SECRET" \
      --tls-certificate /etc/letsencrypt/live/<a-b-c-d>.sslip.io/fullchain.pem \
      --tls-private-key /etc/letsencrypt/live/<a-b-c-d>.sslip.io/privkey.pem \
      wss://[::]:443

In the container:

    wstunnel client -R 'tcp://[::]:6080:127.0.0.1:6080' \
      --http-upgrade-path-prefix "$SECRET" \
      --http-proxy "$HTTPS_PROXY" \
      wss://<a-b-c-d>.sslip.io:443

Then anyone opens `http://<vps>:6080/vnc.html`. Watch for
`SNI DnsName(...)` rather than `SNI IpAddress(...)` in the client log -- the
hostname is what makes the proxy's validation succeed, and connecting by IP
fails even with a valid certificate.

Keep the authtoken or path secret in `/tmp`, never in the repo.

**Never work around this by unsetting `HTTPS_PROXY`.** It is the host's egress
policy, not a misconfiguration.

## The honest fallback

If no tunnel is available, the loop still works: boot, `shot`, `key`, `shot`,
and send the PNGs to the human in the conversation. It is not interactive, but
for "does this actually work before I open a PR" it is most of the value, and
it needs nothing but the container.
