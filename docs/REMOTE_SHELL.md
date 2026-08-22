# Remote Shell

ssh and sftp to a NeoDCT phone, over the internet, without opening it.

Development otherwise means a serial console: two wires soldered to pads
with no strain relief, which come off. This replaces most of what those
wires were for. It does not replace all of it — a phone that will not boot
still needs serial, because Remote Shell lives in the system that failed to
start.

It is off until somebody turns it on, and it is in engineering mode only.

## 1. Why it is shaped like this

The phone is behind carrier-grade NAT and its mobile data is IPv6. Nothing
on the internet can open a connection *to* it. So the phone opens one
*outwards*, to a relay, and asks the relay to carry a way back:

```
NeoDCT  ---- ssh -R, key auth ---->  relay (a VPS with a public address)
                                        ^
your laptop  ---- ssh -J ---------------'
```

Three properties fall out of that, and they are the reason to prefer it
over "just run sshd and forward a port":

- **The phone never listens on a public interface.** `sshd` binds
  `127.0.0.1` and nothing else. The only route in is the tunnel the phone
  dialled itself. Turn Remote Shell on with no relay reachable and there is
  no way in at all — not a closed port, no port.
- **The relay does not publish it either.** The forwarded port lands on the
  relay's loopback, so reaching the phone means getting into the relay
  first.
- **Keys only, in both directions**, and the phone checks the relay's host
  key before handing it the tunnel.

## 2. What you need

- A relay with a public address the phone can reach. **It must have IPv6**
  if the phone is on mobile data — that is what the carrier gives it. The
  cheapest VPS with an AAAA record will do; this needs almost no CPU and
  almost no traffic.
- An ssh keypair for the phone to dial out with.
- Your own ssh keypair, to log in to the phone.
- An SD card, and a few minutes with the phone open, once.

## 3. Set up the relay

Make an account for the phone. It never gets a shell — it only asks for a
port to be forwarded.

```sh
sudo adduser --disabled-password --gecos "" neodct
sudo mkdir -p /home/neodct/.ssh
sudo chmod 700 /home/neodct/.ssh
```

Make the key the phone will dial out with (do this on your machine, not on
the relay — the private half is going to the phone):

```sh
ssh-keygen -t ed25519 -f ./neodct_relay -N "" -C "neodct phone"
```

Put the public half on the relay, restricted so that key can do nothing
except forward a port:

```sh
# on the relay, as root
printf 'restrict,port-forwarding %s\n' "$(cat neodct_relay.pub)" \
    | sudo tee /home/neodct/.ssh/authorized_keys
sudo chown -R neodct:neodct /home/neodct/.ssh
sudo chmod 600 /home/neodct/.ssh/authorized_keys
```

`restrict` turns everything off — no shell, no agent forwarding, no X11,
no pty — and `port-forwarding` turns back on the one thing needed. If that
key ever leaks, what it buys is a port forward on a VPS, not a login.

Check the relay is **not** publishing the forwarded port. Default openssh
already does the right thing; confirm it:

```sh
grep -E '^\s*GatewayPorts' /etc/ssh/sshd_config || echo "GatewayPorts: default (no)"
```

`no` means the forwarded port exists only on the relay's own loopback,
which is what you want. `yes` or `clientspecified` would hang the phone's
sshd off a public port on the VPS, and then the only thing between your
phone and the internet is one key check.

## 4. Put the keys on the phone

The phone has no keyboard, so keys arrive on the card. Make a folder called
`remote` in the root of the card and put three files in it:

| file | what it is |
|---|---|
| `id_ed25519` | the **private** key from step 3, so the phone can dial the relay |
| `authorized_keys` | **your** public key, so you can log in to the phone |
| `known_hosts` | the relay's host key, so the phone knows it is the relay |

Get the relay's host key with:

```sh
ssh-keyscan -p 22 your.relay.example > known_hosts
```

Look at what that returns before you trust it. `ssh-keyscan` asks the
address and writes down whatever answers, which is exactly the thing the
phone is going to rely on — so run it from somewhere you trust the route,
and compare the fingerprint against what the relay prints for itself
(`ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` on the relay).

Then, on the phone: **Engineering → Remote Shell → Copy keys from card**.

The card is FAT and has no permission bits, so everything on it is
world-readable. The phone copies the three files onto its user partition at
`0600` and tells you to delete the originals. Do that — a card in a drawer
with a private key on it is a private key in a drawer.

## 5. Turn it on

On the phone, **Engineering → Remote Shell**:

- **Relay** — the relay's address. An IPv6 literal is fine.
- **Login** — the account from step 3 (`neodct` by default).
- **Port** — the port the relay will listen on, on its own loopback
  (`2222` by default). Any free port; it never faces the internet.
- **Turn on**.

The status line says `On` when both halves are up, `Dialling` when the
local sshd is running but the tunnel is not — which means the relay refused
it, or the phone has no data yet — and `Off` when it is off.

It stays on across restarts until you turn it off. That is deliberate: the
whole point is not needing physical access.

## 6. Connect

```sh
ssh -J you@your.relay.example -p 2222 root@127.0.0.1
```

`-J` connects to the relay first, then from the relay to `127.0.0.1:2222`,
which is the mouth of the tunnel. Files:

```sh
sftp -J you@your.relay.example -P 2222 root@127.0.0.1
```

Worth putting in `~/.ssh/config` so it is one word:

```
Host neodct
    HostName 127.0.0.1
    Port 2222
    User root
    ProxyJump you@your.relay.example
```

Then `ssh neodct` and `sftp neodct`.

The first connection asks you to trust the phone's host key. Check it
against **Remote Shell → This phone's key**, which prints the same
fingerprint on the phone's own screen.

### FileZilla, WinSCP, and file managers

Graphical clients do not understand `ProxyJump` -- it is an ssh idea, not
an SFTP one. Give them a local port instead, and point them at your own
machine. Leave this running while you use them:

```sh
ssh -i ~/.ssh/YOURKEY -N -L 2222:127.0.0.1:2222 you@your.relay.example
```

That opens `127.0.0.1:2222` on *your* machine and pipes it to the relay's
loopback 2222, which is the mouth of the phone's tunnel. Then connect to
`127.0.0.1` port `2222` as `root`, with your key:

| FileZilla field | value |
|---|---|
| Protocol | SFTP - SSH File Transfer Protocol |
| Host | `127.0.0.1` |
| Port | `2222` |
| Logon Type | Key file |
| User | `root` |
| Key file | your private key |

FileZilla offers to convert an OpenSSH key to its own format. Let it; the
original is untouched.

Anything else that speaks SFTP works the same way, including most Linux
file managers via `sftp://root@127.0.0.1:2222`.

## 7. On QEMU

The same thing works, and is easier, because a QEMU phone is not behind
CGNAT. You still need a relay for the tunnel to make sense — or, if you
only want a shell into your own QEMU instance, you do not need Remote Shell
at all: run the emulator with a port forward and reach `sshd` directly.
Remote Shell exists for the hardware problem.

## 8. Known limits

- **The relay's own ssh port is assumed to be 22.** The Port setting in the
  app is the port the *relay listens on for you*, not the port the phone
  dials. If you move sshd on the relay off 22, the phone cannot reach it
  and there is no setting for that yet.

## 9. What this does not protect against

Worth being plain about, because "it uses ssh" invites more confidence than
is earned:

- **The relay can see the traffic pass through but not read it.** The ssh
  session is end to end between your laptop and the phone. The relay
  carries ciphertext. It does know that a phone is connected and when.
- **Whoever holds the relay controls what the phone connects to.** They
  cannot read the session, but they can stop it, and they know your
  fingerprint of activity. Use a relay you own.
- **A card left with keys on it is the weak point**, not the crypto. Anyone
  who takes the card out has the key that dials your relay, and the key
  that logs in to your phone if you left `authorized_keys` there.
- **This is alpha software with a root shell on the far end.** It is a
  development tool. Turn it off when you are not using it.

## 10. Where the code is

| what | where |
|---|---|
| the decisions — config, keys, processes | `System/core/RemoteShell/__init__.py` |
| the switch on the phone | `System/engineering/apps/RemoteShell/main.py` |
| starting it at boot when it was left on | `launcher.py` |
| tests, mostly about who can get in | `neodct/tests/test_remoteshell.py` |

The sshd config is generated from `write_sshd_config()` every single time
it starts, and is not read back. If you edit it on the phone to get past
something, it will be gone the next time Remote Shell starts — change the
source instead, so the next person gets whatever you decided.
