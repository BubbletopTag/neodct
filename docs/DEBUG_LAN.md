# The ethernet debug LAN: a root shell, root file access, and a screen view

A cable between the phone and one development computer, carrying three things
bound to it: a **telnet root shell**, an **ftp server rooted at `/`**, and a
**VNC view of the panel** (`nd-vncd`). This is the "serious debugging" path: it
does not need the relay in `REMOTE_SHELL.md`, it does not need mobile data, and
it works when the UI is dead. `telnet 192.168.99.2` for a shell; point FileZilla
at `192.168.99.2` (any username, no password) for files; point a VNC client at
`192.168.99.2:5900` to watch the screen.

**It is not gated. It runs on any image that boots with an `eth0`, and it is an
unauthenticated cleartext root shell, unauthenticated root-level file
read/write, and an unauthenticated view of the owner's screen.** That is a
deliberate proof-of-concept decision; read "Turning it off" before building an
image that leaves the bench.

## The link

The Luckfox Pico Mini has working 10/100 ethernet. It has no RJ45 and Luckfox
ship `&gmac` disabled, but the MAC and the PHY are both inside the RV1103 and
the RMII pairs are routed -- `HARDWARE_NOTES.md` has the whole finding. Once
`&gmac` is `"okay"` the phone brings up `eth0` and gets carrier.

Addressing, on both ends, is deliberately **not** the home router's subnet:

| end | interface | address |
|---|---|---|
| development host | its own NIC (`enp5s0` on the bench machine) | `192.168.99.1/24` |
| phone | `eth0` | `192.168.99.2/24` |

**Neither end gets a default route on this link, and that is not an
oversight.** The phone's route to the world is the modem's (`S45modem`) and
the host's is its other NIC. If the debug LAN reused the home subnet, the host
would hold two routes for one prefix and router-bound traffic could leave down
the phone cable. Pick a subnet nothing else on the bench uses; `192.168.122.0/24`
(libvirt) and `10.0.2.0/24` are already taken on this machine.

Host side, for one session:

```sh
sudo ip addr add 192.168.99.1/24 dev enp5s0
sudo ip link set enp5s0 up
```

Or permanently, on a NetworkManager host. `never-default` is the part that
matters -- without it NM can install a default route through the phone cable
the moment the profile comes up, which is the failure this whole subnet choice
exists to avoid:

```sh
nmcli con add type ethernet ifname enp5s0 con-name neodct-debug \
      ipv4.method manual ipv4.addresses 192.168.99.1/24 \
      ipv4.never-default yes ipv6.method disabled
nmcli con up neodct-debug
```

The phone side needs nothing at all: it configures itself on every boot.

**The phone's MAC is random on every boot.** There is no MAC in eFuse or in
the device tree, so stmmac generates a locally-administered one. Nothing
host-side may be keyed to it: no DHCP reservations, no `udev` rules, no
firewall rules matching a hardware address.

## What starts it

`/etc/init.d/S42debuglan`, on every boot, with no token and no manual step:

1. does nothing if there is no `eth0`
2. puts `192.168.99.2/24` on `eth0` (or whatever `neodct.telnet=<addr>[/<len>]`
   on the kernel cmdline says instead)
3. **confirms that address is on `eth0`**, and only then starts three daemons,
   each bound to that address:
   - `telnetd -b <addr> -l /bin/sh` on port 23
   - `tcpsvd <addr> 21 ftpd -A -w /` on port 21
   - `nd-vncd --bind <addr> --fps 10` on port 5900 (if the binary is present)

`S41ethernet` stands down when the cmdline pins an address, so DHCP never
races the static one and no lease can arrive carrying a default route.

`neodct.telnet=off` disables the whole link.

### ftp: no login, and why the username is decorative

`ftpd -A` skips the login exchange entirely. busybox `ftpd`'s main loop then
answers every `USER` and every `PASS` with `230 login OK`
(`networking/ftpd.c`), so FileZilla's `USER neodct` succeeds and it never
prompts for a password -- but so would any other name. The name is not a
credential and nothing checks it; treat "username neodct, no password" as "no
authentication at all", which is what it is.

`ftpd` runs as root and is given `/` as its directory, so it `chroot`s to the
real root (a no-op chroot that still succeeds) and `chdir`s there; the client
sees the entire filesystem. `-w` enables writes.

**Reads work everywhere; writes only where the filesystem is writable.** The
rootfs is read-only squashfs, so an upload into `/`, `/etc`, `/usr` and the
rest fails with a permissions/`550` error -- that is the squashfs refusing it,
not ftpd. Push test binaries and anything else you upload to a writable mount:
`/tmp`, `/run`, or `/NeoDCT/User` (the userdata partition). Pulling files off
the phone works anywhere.

`ftpd` has no listening socket of its own; it is an inetd-style service that
talks to one already-connected client on stdin/stdout. `tcpsvd` provides that
socket and is what binds the address -- see below.

### nd-vncd: a view of the panel

`nd-vncd` (`neodct/src/vncd/`, built into the image, installed at
`/NeoDCT/System/bin/nd-vncd`) serves `/dev/fb0` over RFB. fb0 is the vfb the UI
draws into and `neodct_displayd` mirrors to the ST7789, so a VNC client sees
exactly what is on the panel, byte for byte. It is **view-only** in v1 -- see
"Input" below -- and serves the framebuffer's 240x175, not the panel's 240x240.

It is the display daemon's skeleton with libvncserver as the sink: it reads the
real pixel format from `fb_var_screeninfo` (never assumes a byte order -- the
red/blue trap that cost a release lives in a comment block in the source),
diffs frames into a dirty rectangle, converts only that rectangle into an RGBX
buffer, and sends it. An idle screen sends nothing and costs ~zero CPU; with no
client it does not poll at all. Default 10 fps (`--fps N`) to suit the
10 Mbit/s link. `--swap-rb` inverts the detected red/blue order for a driver
that fills the struct in wrongly. Its startup line logs geometry, bpp, stride,
channel offsets and bind address -- when "the colours are wrong", that line is
the diagnosis.

Like telnet and ftp it binds one address and never the modem: `--bind <addr>`
pins the IPv4 socket, and it sets libvncserver's `ipv6port = 0` so there is no
IPv6 listener at all. Its own default `--bind` is loopback, so a hand-run with
no arguments cannot expose the screen to the network.

**Input is out of scope for v1, on purpose.** A uinput device fed from VNC key
events does not work here: apps do not read `/dev/input`, they read a pipe the
core hands them (`NEODCT_KEYPAD_FD`), and `nd_input.c`'s `is_our_injector()`
deliberately refuses the one uinput device the core creates. Key injection
belongs at the core's key source -- a dev-only channel `nd_input` polls
alongside the i2c matrix. `nd-vncd` is written with that hook as a clearly
marked, unimplemented seam (`screen->kbdAddEvent`), so wiring it later is a
dozen lines.

### Why all three are bound, and why that part is not provisional

Everything else here is proof-of-concept and expected to change. The bind is
not.

A daemon with no bind address listens on `0.0.0.0` **and** `::` -- every
interface the phone has, including `wwan0`, the modem. The carrier is CGNAT
today, so an unbound daemon would probably not be reachable from outside.
"Probably not reachable" is not a security property: CGNAT is an addressing
accident, not a firewall, and it changes when the APN, the carrier or the SIM
does. A bound socket does not care.

So the address is always explicit (`telnetd -b <addr>`, `tcpsvd <addr>`,
`nd-vncd --bind <addr>`), it is always one the script has just confirmed is on
`eth0` (`ip -4 addr show dev eth0`, which cannot be satisfied by an address
belonging to `wwan0`), and if that check fails no daemon starts. Each also
fails closed at bind time if the address somehow is not local -- `telnetd`'s
`create_and_bind_stream_or_die`, `tcpsvd`'s `bind` getting `EADDRNOTAVAIL`, and
`nd-vncd`'s `rfbInitServer` failing to listen (plus its `ipv6port = 0`, so no
`::` socket exists to reach over the modem). There is no path through
`S42debuglan` that ends in a daemon on the modem.

### The gate that is coming

The intended gate is the engineering-mode marker this project already has:
`/etc/neodct-devenv`, written by `neodct/scripts/post-build-devenv-marker.sh`
only when the build asks for it (`NEODCT_DEVENV_IMAGE=1 make`), living in a
read-only squashfs under dm-verity so a running phone cannot create it.

`post-build-devenv-marker.sh` sets out why that shape is the right one: the
gate must not be reachable from writable storage, or it is
`docs/c-rewrite/SECURITY-AUDIT.md` section 4 Q5 vector 2 with a root shell on
the end of it. Wrapping the `start)` case in a test for that file is the whole
change.

## Using it

A shell:

```sh
telnet 192.168.99.2
```

You get root, immediately, with no password. Files, with any ftp client:

```sh
ftp 192.168.99.2          # or point FileZilla at it
#   Host: 192.168.99.2   Port: 21   User: anything   Password: (blank)
```

In FileZilla this is a **Plain FTP** connection (not FTPS/SFTP), and its
"insecure, continue?" warning is correct -- say yes. You land at `/` with the
whole rootfs visible and writable. And the screen, with any VNC client:

```sh
vncviewer 192.168.99.2:5900     # or gvncviewer, or noVNC in a browser
```

No password; you get a 240x175 live view. It updates only when the screen
changes, so a still home screen is silent on the wire. A black view where the
phone seems on may just be the backlight asleep -- fb0 keeps its last contents
(nd-vncd logs a note about this at startup).

Nothing on this link is encrypted; anything typed, transferred or displayed is
on the wire in clear. This is a bench tool on a cable between two machines and
it is not fit for anything else.

To check the link from the host without any phone-side configuration at all:

```sh
ip neigh show dev enp5s0     # the phone answers IPv6 NDP as soon as eth0 is up
```

An `INCOMPLETE` entry for `192.168.99.2` means the host ARPed and got no
answer -- the phone has carrier but no address yet.

## Turning it off

Set `neodct.telnet=off` in `NEODCT_BOOTARGS_EXTRA` in the SDK board config,
then `./build.sh env && ./build.sh updateimg` and reflash. All three daemons
stay in the image -- they are a few tens of kb of busybox plus nd-vncd -- but
nothing starts them and no port opens.

**Until the engineering-mode gate lands, that is a thing to remember rather
than a thing the build enforces.** No image intended for anyone other than a
developer at a bench should ship as it stands. To remove the daemons outright:
the `CONFIG_TELNETD*`, `CONFIG_FTPD*` and `CONFIG_TCPSVD` lines at the end of
`buildroot/board/qemu/busybox.fragment` (telnet + ftp), and
`BR2_PACKAGE_LIBVNCSERVER*` in the defconfigs (nd-vncd, which then does not
build). The nearer-term gate is `nd-vncd --require-devenv` plus the same test
wrapping `S42debuglan`'s `start)` case.

## What this does not change

`sshd` is still loopback-only. RemoteShell regenerates `sshd_config` with
`ListenAddress 127.0.0.1` on every start (`nd_remoteshell.c`), pinned by a unit
test that says "loopback only, never an interface". Ethernet does not open ssh
to the phone and this document does not propose that it should. telnet, ftp and
VNC are the weaker things in every respect -- no authentication, no encryption,
and for now no gate -- which is exactly why they are pinned to one address on
one interface, and why widening ssh is a separate decision rather than a
side effect of this one.
