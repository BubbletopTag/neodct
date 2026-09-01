# SIM7600G Modem Bring-Up (0.3.0a)

How NeoDCT talks to the SIM7600G-H on T-Mobile, from raw AT pokes to live
signal bars on the home screen. No ModemManager, no libqmi, no PPP — the
whole stack is BusyBox + the kernel's `option` and `qmi_wwan` drivers +
~400 lines of Python.

## The stack

| Layer | Piece | Job |
|-------|-------|-----|
| kernel | `option` driver | `1e0e:9001` is in its device table (SIM7100/7600 entry) → auto-binds, creates `/dev/ttyUSB0-4` |
| kernel | `qmi_wwan` + `usb_wdm` | `wwan0` network interface at the stock PID, no mode-switching |
| shell | `/NeoDCT/System/engineering/tools/atcmd` | one scriptable AT command per call, lock-safe |
| boot | `/etc/init.d/S45modem` | backgrounded auto-connect: SIM → register → `AT$QCRMCALL` → SLAAC |
| UI | `System/core/ModemService` | owns the AT port from Python; CSQ → home-screen bars, call control, URC events |

Port map at the stock PID `1e0e:9001`:

| Node | Interface | Purpose |
|------|-----------|---------|
| `/dev/ttyUSB0` | 0 | Qualcomm DIAG (binary — leave it alone) |
| `/dev/ttyUSB1` | 1 | GPS NMEA stream |
| `/dev/ttyUSB2` | 2 | **AT commands** ← everything here uses this |
| `/dev/ttyUSB3` | 3 | AT / PPP modem port (spare) |
| `/dev/ttyUSB4` | 4 | **call audio PCM** (16 kHz mono S16_LE after `AT+CPCMREG=1`) |

## QEMU passthrough recap

```
-device qemu-xhci,id=xhci
-device usb-host,bus=xhci.0,vendorid=0x1e0e,productid=0x9001
```

* `chmod 666 /dev/bus/usb/BBB/DDD` on the host (avoids running QEMU as
  root, which breaks PulseAudio).
* **Stop ModemManager on the host first** (`systemctl stop ModemManager`):
  it grabs the AT ports the moment the modem enumerates and QEMU may
  detach the device mid-conversation, leaving the firmware confused.
* **Never `AT+CUSBPIDSWITCH`** while using QEMU passthrough: the modem
  re-enumerates with a new product ID and the `productid=0x9001` filter
  stops matching, so the modem vanishes from the guest. (Also the reason
  we use `$QCRMCALL` instead of RNDIS mode.)
* Hardware note: until the blown CapXon cap is bodged (470–1000 µF ≥6.3 V
  across 5 V/GND), expect possible brownout reboots during LTE TX bursts.
  A mid-session reboot looks like all `ttyUSB*` nodes vanishing and
  re-enumerating — `dmesg` will show the disconnect.

## Stage 1 — terminal proof of concept

`atcmd` ships in the image at `/NeoDCT/System/engineering/tools/atcmd`.
Sanity checks first:

```sh
alias at='/NeoDCT/System/engineering/tools/atcmd'
at AT              # -> OK              (modem alive)
at ATI             # -> SIM7600 model info
at AT+CPIN?        # -> +CPIN: READY    (SIM detected/unlocked)
at AT+CSQ          # -> +CSQ: 20,99     (rssi 0-31; the 99 here is BER)
at AT+CEREG?       # -> +CEREG: 0,1     (…,1 home / …,5 roaming = registered)
at AT+COPS?        # -> +COPS: 0,0,"T-Mobile",7
at AT+CPSI?        # -> +CPSI: LTE,Online,…,-1134,…  (field 12 is RSRP x10)
at AT+CEER         # -> why the last attach/PDN failed, when one did
```

`+CSQ: 99,99` is the modem declining to give a reading, which is not the
same as a bad one: on LTE many firmwares answer 99 permanently. `AT+CPSI?`
is the measurement to trust — and if a phone is showing "no signal", ask
`AT+CPIN?` and `AT+CEER` before the antenna, because a locked SIM and a
refused registration look identical from the CSQ row.

Then the data call (T-Mobile is IPv6-only with NAT64/DNS64). APN matters:
our Tello SIM uses **`wholesale`**; a native T-Mobile SIM wants
`fast.t-mobile.com` (S45modem walks both automatically, see Stage 2):

```sh
at 'AT+CGDCONT=1,"IPV6","wholesale"'
at -t 15 'AT$QCRMCALL=1,1'          # firmware starts the RmNet call
ip link set wwan0 up
sysctl -w net.ipv6.conf.wwan0.accept_ra=2
sleep 5                              # SLAAC: address arrives via RA
ip -6 addr show dev wwan0            # want a "scope global" inet6 here
ping -6 -c 3 2606:4700:4700::1111    # packets over a REAL TOWER
```

DNS for name resolution (Google DNS64, synthesizes AAAA for v4-only hosts):

```sh
echo 'nameserver 2001:4860:4860::6464' >> /etc/resolv.conf
```

Useful extras: `AT+CGPADDR=1` (modem's view of the address),
`AT+CGCONTRDP=1` (address + T-Mobile's own DNS servers),
`AT$QCRMCALL?` (call state), `AT$QCRMCALL=0,1` (tear down).

## Stage 2 — automatic connection at boot

`/etc/init.d/S45modem` runs the whole Stage-1 sequence in the background
at every boot (boot itself never blocks): waits for the AT port, checks
the SIM, waits up to 90 s for LTE registration, then dials and brings up
`wwan0`. The bring-up is self-healing:

* **ESM settle** — waits 8 s after `+CEREG: 0,1` before dialing.
  Dialing instantly gets `+CEER: ESM sync up with network` rejects
  while the attach (including the IMS PDN that carries VoLTE) is still
  being set up.
* **Ride the attach bearer first** — each round starts with `AT+CGACT?`;
  if the network already activated a non-IMS context (seen live as
  `+CGACT: 2,1`), `$QCRMCALL` starts on *that* cid without redefining
  anything — the network can reject a second PDN request while happily
  serving the first.
* **APN walk** — the cached winner, the modem's own cid1 definition
  (whatever ModemManager last connected with), `wholesale` (Tello —
  confirmed against the host's working NetworkManager profile), then
  `fast.t-mobile.com` and `tello.com`.
* **PDP-type walk** — each APN is tried as `IPV6`, then `IPV4V6` (the
  host's proven ModemManager bearer was ipv4v6; some plans reject
  v6-only contexts with an instant `NO CARRIER`).
* **NO CARRIER = try again** — weak RF (the basement) rejects calls
  that succeed moments later. Every dial retries once, the whole walk
  runs `MODEM_DIAL_ROUNDS` (4) times with a PS re-attach between rounds
  (`AT+CGATT=0/1` — also required for a rewritten cid1 to take effect),
  and after that the worker idles and redials forever every
  `MODEM_REDIAL_IDLE_S` (120 s), like the phone app does. Every
  rejection logs `AT+CEER` (the network's actual reject cause) and
  `AT+CPSI?` (real RSRP, far more honest than CSQ indoors).
* **Automatic raw-ip** — if the call is up but no SLAAC address arrives
  in 20 s, qmi_wwan framing is flipped to raw-ip and the call re-dialed
  (many SIM7600 firmwares only speak raw-ip; no manual toggle needed).
* **QMI fallback (uqmi)** — each round ends with
  `uqmi --start-network --apn wholesale` on `/dev/cdc-wdm0` — the exact
  mechanism ModemManager used for the proven host connection, in case
  `$QCRMCALL` is the broken layer.
* **Neither the SIM nor registration is fatal** — both used to `fail`, which
  exits the worker. A SIM subsystem two seconds slower than the AT
  interpreter (`+CME ERROR: 14`, SIM busy) or ninety seconds without a tower
  therefore cost the phone its data connection for the whole session, and
  none of the self-healing below ever ran. The SIM is now waited for
  (`MODEM_SIM_WAIT_S`, 20 s), a locked SIM is reported rather than retried
  forever, and a phone that is not registered falls into the redial loop —
  which re-checks registration every round and every 120 s after that — with
  `AT+CPIN?`, `AT+CEER`, `AT+CPSI?`, `AT+CSQ` and `AT+COPS?` in the log
  saying why.
* **Module reset once** — if a whole dial cycle fails, `AT+CFUN=1,1`
  reboots the module and everything is retried. Firmware state survives
  USB passthrough: a QMI/WDS session left by host-side ModemManager can
  make every `$QCRMCALL` fail instantly with a stale
  `+CEER: EMM detached` even though CEREG shows registered and bearers
  are active. `MODEM_RESET_ON_FAIL=n` disables it.
* **Carrier DNS** — `AT+CGCONTRDP` (in `AT+CGPIAF` colon notation) puts
  T-Mobile's own DNS64 servers into resolv.conf first; the Google DNS64
  anycast stays as fallback.
* **Success cache** — the APN + framing that worked land in
  `/etc/modem.cache`, so the next boot connects on the first try.
  Delete the file to re-probe from scratch.

* Watch it work: `tail -f /tmp/modem-boot.log`
* One-word answer: `cat /tmp/modem.status` → `up:wwan0` / `failed: …`
* Re-run by hand: `/etc/init.d/S45modem restart`

Overrides live in `/etc/default/modem` (plain sh, all optional):

```sh
MODEM_APN="wholesale"        # first configured APN (Tello data APN)
MODEM_APN_FALLBACKS="fast.t-mobile.com tello.com"
MODEM_PDPTYPE="IPV6"         # preferred context type
MODEM_PDPTYPE_FALLBACKS="IPV4V6"
MODEM_PORT="/dev/ttyUSB2"
MODEM_RAW_IP="auto"          # auto-detected; "y"/"n" to pin the framing
MODEM_DIAL_ROUNDS=4          # APN-walk rounds before going to slow retry
MODEM_DIAL_RETRY_WAIT_S=15   # pause between rounds
MODEM_REDIAL_IDLE_S=120      # forever-retry cadence after the rounds
```

## Stage 3 — the software (ModemService)

`System/core/ModemService` now drives the real modem (same
probe-or-simulate pattern as BatteryService):

* **Live signal bars**: polls `AT+CSQ` every 5 s; the home-screen `sig`
  icon_set renders 0–4 real bars (not registered → 0 bars). In QEMU
  without passthrough it stays on the layout's `sim_val`.
* **Port auto-probe**: prefers USB interface 2, then 3 (read from sysfs),
  confirms with `AT`→`OK`; re-probes every 10 s if the modem is missing,
  so passthrough/hotplug attach after boot is adopted automatically.
* **Live carrier line**: the home screen's "No Service" text becomes the
  real operator name ("Tello" / "T-Mobile", short-format `AT+COPS=3,1`)
  once registered; it falls back to "No Service" whenever registration
  drops.
* **Calls are LIVE and FULL-DUPLEX**: `system.modem.allow_calls`
  defaults ON (OFF restores the pretend flow). `dial()` sends `ATD`,
  then immediately `AT+CPCMREG=1`, then runs two alsa-utils pipes on the
  bidirectional PCM port: `aplay -t raw -f S16_LE -r 16000 -c 1
  /dev/ttyUSBn` (downlink → speaker) and `arecord … -D <mic> /dev/ttyUSBn`
  (USB sound card mic → uplink). Mic device:
  `system.hw.modem_mic_device` ("default"; `plughw:N,0` to pin;
  OFF = listen-only; three arecord failures auto-degrade to listen-only
  — check `arecord -l` and capture levels with `amixer`). Teardown
  (End key, `NO CARRIER`, `VOICE CALL: END`) kills both pipes and sends
  `AT+CPCMREG=0`; the call screen exits by itself on remote hangup.
  Typing a number plays fake DTMF beeps (`System/tones/dtmf/`). Test
  line: 1-800-444-4444 (MCI readback — it reads your voice channel back,
  so hearing your own mic echo confirms full duplex!). QEMU note: if
  `arecord -l` in the guest is empty, QEMU's usb-audio is playback-only
  on your build — use `-device virtio-sound-pci,audiodev=audio0`
  instead/alongside (kernel already has `CONFIG_SND_VIRTIO=y`).
  Bench note: LTE TX during a call is the worst case for the missing
  capacitor — if the modem vanishes mid-call, that's the brownout.
* **Coexistence**: every port transaction takes the advisory lock
  `/tmp/neodct-modem.lock`, shared with `atcmd` and `S45modem` — you can
  poke AT commands from the serial console while the UI runs.
* **Sim hooks** (QEMU, no modem): `echo 23 > /tmp/neodct_sim_csq` drives
  the bars; `echo 5551234 > /tmp/neodct_sim_ring` fakes an incoming call
  (`rm` it to "hang up"); `echo Tello > /tmp/neodct_sim_operator` fakes
  the carrier line; dialing auto-"connects" after 2 s.
* **SMS sending is LIVE** (the first real telephony): Messages → Write
  Message → Options → Send prompts for a number (arrow keys open the
  PhoneBook contact picker) and sends via `ModemService.send_sms()` —
  text-mode `AT+CMGS` with special handling for the newline-less `>`
  prompt.
* **SMS receiving is LIVE, 3310-style** (NotifyService): `AT+CNMI`
  pushes `+CMTI` the instant a message lands → fetched with `AT+CMGR`,
  deleted from the SIM, stored in the inbox DB. The original Nokia tone
  beeps immediately; the home screen shows "N message(s) received" with
  the **Read** softkey (C dismisses) and a flashing envelope while
  unread mail exists. A boot sweep (`AT+CMGL="REC UNREAD"`) imports
  messages that arrived while the phone was off. QEMU sim hook:
  `echo '5551234|hey' > /tmp/neodct_sim_sms`.
* **Incoming calls ring and interrupt apps**: `RING`/`+CLIP` →
  `IncomingCall` (a `BaseException`, like `KeyboardInterrupt`) raised
  from `read_keypress` unwinds whatever app is running (their `finally`
  blocks release ALSA), then the 3310-style screen shows the caller with
  a flashing "calling", **Answer** softkey and **C** to decline. The
  ringtone is `system.audio.ringtone`, looped in-process via miniaudio.
  QEMU sim hook: `echo 16165551234 > /tmp/neodct_sim_ring` rings once
  per write/touch; `rm` it to make the caller give up.
* **Modem engineering app** (menu id 9005): RADIO / SIM / DATA status
  pages, softkey walks Next→Next→Exit. Shows operator, CEREG, CSQ+dBm,
  CPIN, phone number, IMEI, ICCID, IMSI, firmware, S45modem status, wwan
  interface, IPv6, APN and DNS — troubleshooting without a serial
  console. In Simulation Mode it lists which ttyUSB nodes exist instead
  of bailing out.

  The RADIO page has room for six rows and spends the sixth on whatever
  is actually wrong. On a phone that is **not registered** it drops BARS
  (a hard 0 for every non-registered CEREG stat, so the row is a constant)
  and shows **SIM** (`AT+CPIN?`) and **CAUSE** (`AT+CEER`) instead, and the
  CSQ row shows `AT+CPSI?`'s serving cell in place of "99 (no signal)". On
  a phone with **no modem** it drops CALL (a forced IDLE) and shows **WHY**
  — the probe's own reason, which is carried across the service wire for
  that purpose and, before this, never fitted on the page.

  ModemService polls those three from the core, one per free tick every
  10 s, because an app process cannot: raw AT is deliberately not on the
  service wire, so the SIM page's own six transactions answer nothing on
  real hardware. They ride the status snapshot instead.

## Stage 4 — proving the full stack (modem = the internet)

`scripts/qemu_modem_data_test.py` is the one-command end-to-end proof.
It boots the image headless (`-snapshot`, so rootfs.ext4 is untouched)
with the modem passed through, logs in over the serial socket, waits for
S45modem's verdict, then checks: wwan0 global IPv6, default route,
ping to a real v6 literal, DNS resolution, HTTP fetch, HTTPS fetch (the
exact path NetSurf uses — libcurl + the target CA bundle).

```sh
# host, from neodct/: close any other QEMU using the modem first
scripts/qemu_modem_data_test.py            # → RESULT: PASS/FAIL + boot log
```

Manual equivalent inside any running guest (serial console):

```sh
/etc/init.d/S45modem restart
sleep 60; cat /tmp/modem.status; tail -30 /tmp/modem-boot.log
ping -6 -c 2 2606:4700:4700::1111    # image ≥ this commit; else use curl
curl -sI https://example.com          # DNS64 + NAT64 + TLS, full browser path
```

Once `modem.status` says `up:wwan0`, NetSurf needs nothing special: it
resolves via resolv.conf (DNS64 synthesizes AAAA for v4-only sites) and
routes over wwan0's SLAAC default route. In a QEMU that *also* has
usernet `eth0`, IPv6 wins by address-sort, so traffic still prefers the
modem — unplug eth0 (or boot the modem-only QEMU command) to be sure.

## Troubleshooting

| Symptom | Meaning / fix |
|---------|---------------|
| no `/dev/ttyUSB*` | `lsusb` shows `1e0e:9001`? If yes: kernel missing `option` driver (needs the 0.3.0a image). If no: passthrough/cable/power. |
| interface isn't called `wwan0` | Normal — eudev predictable naming renames it (QEMU: `wwp0s5u3i5`). S45modem and the Modem app find it by the `qmi_wwan` driver, not the name; substitute your name in any manual command here. |
| `+CSQ: 99,99` forever | 99 is "no reading", which is not the same as "no signal" — many SIM7600 builds never map an RSSI from LTE at all and answer 99 for the entire time the phone is camped and working. Read `AT+CPSI?` before believing it (the Modem app's CSQ row now shows the serving cell in place of the 99). A 99 **with** `+CPSI: NO SERVICE` is the real thing: antenna, or a brownout reboot (cap bodge). |
| `+CEREG: 0,2` stuck | Searching. Check antenna + `AT+CPIN?`; give it 1–2 min. |
| `+CEREG: 0,3` | Registration **denied** — the network is refusing this SIM/IMEI, not failing to hear it. `AT+CEER` names the cause and each points somewhere different: #11 PLMN not allowed, #13 roaming not allowed here, #33 service option not subscribed (plan/APN), #5 IMEI not accepted. Usually an unactivated or wrong-carrier SIM. The Modem app's CAUSE row is this reading. |
| `+CEREG: 0,4` | Unknown — the modem is not seeing a usable E-UTRAN cell. On its own it is coverage (or an antenna); if it later becomes a 3, the modem HAS found the network and been refused, so read the 0,3 row. |
| RADIO page says `REG UNKNOWN` / `CSQ 99` and nothing else | Older image. Since the CPIN/CEER/CPSI rotation the page adds SIM and CAUSE rows whenever the phone is not registered, and the CSQ row carries `+CPSI?`'s serving cell — that is where the answer is. |
| `$QCRMCALL` OK but no SLAAC address | S45modem now flips raw-ip framing by itself; if hand-testing: `ip link set wwan0 down; echo Y > /sys/class/net/wwan0/qmi/raw_ip; ip link set wwan0 up` and re-dial. |
| `$QCRMCALL` → `NO CARRIER` instantly | Read the `cause:` (AT+CEER) lines in `/tmp/modem-boot.log`. "ESM sync up with network" = dialed too soon / second-PDN collision — the ride-the-attach-bearer path and settle delay handle it. "EMM detached" while CEREG=1 and `cgact:` shows active bearers = stale firmware state (usually host ModemManager's leftover QMI session) — the automatic `AT+CFUN=1,1` reset clears it. Cause #33 "service option not subscribed" = APN/plan. RF causes clear on retry — the worker keeps redialing every 2 min on its own. Check `cell:` (CPSI): field 12 is RSRP×10 (−1134 = −113.4 dBm; below ≈ −115 is cell-edge), field 11 RSRQ×10 (−185 = −18.5 dB, poor). |
| `$QCRMCALL` broken no matter what | The QMI path is the escape hatch (it's what ModemManager uses): `uqmi -d /dev/cdc-wdm0 --start-network --apn wholesale --ip-family ipv6`, then watch for SLAAC. S45modem tries this automatically at the end of every round. |
| every APN fails (`no data call yet`) | Wrong APN for the SIM (Tello = `wholesale`) or the plan has no data. Check `/tmp/modem-boot.log` for which were tried; delete `/etc/modem.cache` after changing plans. The proven-good APN is whatever the host's NetworkManager profile used: `nmcli -g gsm.apn connection show <name>`. |
| ping works, names don't | resolv.conf lacks a DNS64 server (Stage 1 step). |
| `ping -6` says invalid option | Image older than the busybox `PING6` fragment — use `curl -g 'http://[2606:4700:4700::1111]/'` instead. |
| `atcmd: modem port busy` | S45modem mid-registration or UI polling — it clears in seconds. |
| everything vanished mid-test | Modem rebooted (power) or host re-grabbed it (ModemManager). |
