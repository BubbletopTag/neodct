"""ModemService -- SIM7600 AT-command driver with a QEMU simulation fallback.

Owns the modem's AT port (default /dev/ttyUSB2, override with the
system.hw.modem_at_port setting) using raw termios + non-blocking reads:
no pyserial, no ModemManager, no threads. The main loop pumps poll()
through NeoDCT_UI._modem_tick() (same pattern as BatteryService), which

  * drains unsolicited result codes (RING, +CLIP, VOICE CALL: BEGIN/END,
    NO CARRIER) into the call state machine and a small event queue,
  * refreshes signal quality (AT+CSQ) every POLL_SIGNAL_S seconds --
    signal_level() feeds the home screen "sig" icon set with 0..4 bars,
  * refreshes registration/operator (AT+CEREG? / AT+COPS?) more slowly.

Port access is serialized against the S45modem boot script and the atcmd
helper with an advisory flock on LOCK_FILE. Whoever holds the lock wins;
everyone else skips that transaction and retries on a later tick, so the
UI never garbles the boot script's registration dance.

SMS sending is live via send_sms() (text-mode AT+CMGS with bespoke
">"-prompt handling). Receiving is push-based, not polled: AT+CNMI makes
the modem raise a +CMTI URC the moment a message lands, which becomes an
("sms_received", index) event; main.py then fetch_sms()es it into the
inbox DB and NotifyService beeps. read_stored_sms() sweeps messages that
arrived while the phone was off (queued as ("sms_stored_check", None)
right after the modem is adopted). Fetched messages are deleted from the
SIM so its ~30 slots never fill up and silently stop reception.

Calls are LIVE and FULL-DUPLEX: dial() sends ATD, then immediately
AT+CPCMREG=1 to route call audio over USB (the PCM port on interface 4
is bidirectional), then runs two fire-and-forget alsa-utils pipes:

    speaker <- aplay   <- PCM port    (the far end's voice)
    far end <- PCM port <- arecord    (our mic, via the USB sound card)

Zero in-process audio code. The mic ALSA device comes from
system.hw.modem_mic_device (default AUTO = first card that can capture,
found via /proc/asound -- QEMU's playback-only USB Audio card makes
"default" a trap; set OFF for listen-only). No capture device at all
degrades cleanly to listen-only. Call end (VOICE CALL: END / NO CARRIER
/ hangup) kills
both pipes and schedules AT+CPCMREG=0. The system.modem.allow_calls
setting defaults ON; set it OFF to fall back to the old pretend flow
(CALLING -> CONNECTED after 2 s, no AT traffic).

If no AT port answers, the service runs in Simulation Mode (boot log says
so, like BatteryService) and quietly re-probes every PROBE_RETRY_S, so a
modem hotplugged later is adopted automatically. Sim hooks for QEMU
without passthrough:
  echo 23 > /tmp/neodct_sim_csq         # drive the signal bars (0-31, 99)
  echo 5551234 > /tmp/neodct_sim_ring   # fake an incoming call (rm = hangup)
  echo Tello > /tmp/neodct_sim_operator # fake the home-screen carrier line
  echo '5551234|hey there' > /tmp/neodct_sim_sms   # fake a received SMS
"""

import fcntl
import os
import subprocess
import termios
import time
from collections import deque

LOCK_FILE = "/tmp/neodct-modem.lock"
ASOUND_DIR = "/proc/asound"
SIM_CSQ_FILE = "/tmp/neodct_sim_csq"
SIM_RING_FILE = "/tmp/neodct_sim_ring"
SIM_OPS_FILE = "/tmp/neodct_sim_operator"
SIM_SMS_FILE = "/tmp/neodct_sim_sms"

DEFAULT_PORT = "AUTO"          # AUTO probes ttyUSB2/3 first, then the rest
BAUD = termios.B115200

POLL_URC_S = 0.5               # drain unsolicited lines this often
SMS_PROMPT_TIMEOUT_S = 5.0     # AT+CMGS -> ">" prompt
SMS_SEND_TIMEOUT_S = 30.0      # body+Ctrl-Z -> network ack (+CMGS/OK)
POLL_SIGNAL_S = 5.0            # AT+CSQ cadence
POLL_NET_S = 20.0              # AT+CEREG? cadence
POLL_OPERATOR_S = 60.0         # AT+COPS? cadence
PROBE_RETRY_S = 10.0           # sim mode: how often to look for hardware

# CSQ rssi (0..31, 99=unknown) -> 0..4 bars, roughly -105/-93/-81/-73 dBm.
BAR_THRESHOLDS = (2, 8, 14, 20)

# Call audio over USB: the SIM7600's PCM port (USB interface 4) streams
# mono signed-16-bit little-endian once AT+CPCMREG=1 is set. The rate is
# firmware-configurable via AT+CPCMFRM (0 = 8 kHz factory default,
# 1 = 16 kHz); we force 16 kHz to match aplay. If your firmware rejects
# CPCMFRM (logged at dial time), set system.hw.modem_pcm_rate=8000.
PCM_FORMAT = "S16_LE"
CLCC_POLL_S = 2.0              # call-progress query cadence during a call
AUDIO_RESTART_HOLDOFF_S = 3.0  # min gap between aplay restart attempts

# AT+CLCC <stat> values.
CLCC_STATES = {0: "CONNECTED", 1: "HELD", 2: "CALLING", 3: "RINGING",
               4: "INCOMING", 5: "WAITING"}

FINAL_CODES = ("OK", "ERROR", "NO CARRIER", "NO DIALTONE", "BUSY", "NO ANSWER")

# Unsolicited lines we route to the state machine even mid-command.
URC_PREFIXES = ("RING", "+CLIP:", "VOICE CALL:", "MISSED_CALL:", "NO CARRIER",
                "+CMTI:", "+CEREG:", "+CREG:", "+CPIN:", "+SIMCARD:")


class ModemService:
    def __init__(self, port=None):
        print("[MODEM] Initializing ModemService...")

        self.state = "IDLE"    # IDLE, CALLING, RINGING, CONNECTED
        self.hardware = False
        self.port = None
        self.fd = None
        self.imei = None
        self.operator = None
        self.caller_id = None

        self._csq = None           # last rssi 0..31, 99/None = unknown
        self._reg_stat = None      # +CEREG <stat>: 1 home, 5 roaming
        self._rxbuf = b""
        self._events = deque(maxlen=8)
        self._next_urc = 0.0
        self._next_csq = 0.0
        self._next_net = 0.0
        self._next_cops = 0.0
        self._next_probe = 0.0
        self._sim_connect_at = None
        self._audio_proc = None     # aplay: PCM port -> speaker (downlink)
        self._mic_proc = None       # arecord: mic -> PCM port (uplink)
        self._active_pcm_port = None
        self._mic_fails = 0
        self._pcm_cleanup = False   # send AT+CPCMREG=0 on a later tick
        self._pcm_retry = False     # re-assert CPCMREG=1 once connected
        self._call_stat = None      # last AT+CLCC <stat> during a call
        self._call_connected_at = None
        self._next_clcc = 0.0
        self._next_audio_restart = 0.0
        self._sim_ring_mtime = None
        self._pcm_rate = self._pcm_rate_setting()
        self._configured_port = port or self._port_from_settings()
        self._allow_calls = self._calls_enabled_setting()

        self._lock_fd = os.open(LOCK_FILE, os.O_RDWR | os.O_CREAT, 0o666)

        if not self._probe_hardware():
            print("[MODEM] HARDWARE NOT FOUND: Running in Simulation Mode.")
            print("[MODEM] Will re-probe every %ds; sim hooks: %s / %s."
                  % (PROBE_RETRY_S, SIM_CSQ_FILE, SIM_RING_FILE))

    # --- setup -----------------------------------------------------------

    @staticmethod
    def _port_from_settings():
        try:
            from System.core.SettingsStorage import get_setting
            return str(get_setting("system.hw.modem_at_port", DEFAULT_PORT))
        except Exception as exc:
            print(f"[MODEM] Settings unavailable ({exc}); probing default ports.")
            return DEFAULT_PORT

    @staticmethod
    def _pcm_rate_setting():
        try:
            from System.core.SettingsStorage import get_setting
            return int(str(get_setting("system.hw.modem_pcm_rate", "16000")))
        except Exception:
            return 16000

    @staticmethod
    def _calls_enabled_setting():
        # Default flipped to ON for the 0.3.0a call bring-up: the USB
        # audio path exists now. Set system.modem.allow_calls=OFF to get
        # the old pretend flow back.
        try:
            from System.core.SettingsStorage import get_setting
            val = str(get_setting("system.modem.allow_calls", "ON"))
            return val.strip().upper() in ("ON", "1", "TRUE", "YES")
        except Exception:
            return True

    def _candidate_ports(self):
        if self._configured_port and self._configured_port != "AUTO":
            return [self._configured_port]
        # On the SIM7600 (1e0e:9001) interface 2 is the AT port and 3 the
        # modem port; prefer them, then try whatever else enumerated.
        preferred, rest = [], []
        for name in sorted(os.listdir("/sys/class/tty") if os.path.isdir("/sys/class/tty") else []):
            if not name.startswith("ttyUSB"):
                continue
            dev = "/dev/" + name
            try:
                with open("/sys/class/tty/%s/device/../bInterfaceNumber" % name) as f:
                    iface = int(f.read().strip(), 16)
            except Exception:
                iface = None
            if iface in (2, 3):
                preferred.append((iface, dev))
            elif iface != 0:   # interface 0 is the diag port; skip it
                rest.append(dev)
        return [dev for _, dev in sorted(preferred)] + rest

    def _probe_hardware(self):
        """Try to adopt an AT port. Returns True on success."""
        self._next_probe = time.monotonic() + PROBE_RETRY_S
        if not self._acquire():
            return False   # S45modem is mid-session; retry on a later tick
        try:
            return self._probe_ports()
        finally:
            self._release()

    def _probe_ports(self):
        for dev in self._candidate_ports():
            if not self._exists(dev):
                continue
            fd = None
            try:
                fd = self._open_port(dev)
            except Exception:
                continue
            self.fd, self.port = fd, dev
            final, _ = self._transact("AT", timeout=1.0)
            if final == "OK":
                self._init_modem()
                return True
            self.fd, self.port = None, None
            try:
                os.close(fd)
            except Exception:
                pass
        return False

    @staticmethod
    def _exists(path):
        try:
            return os.path.exists(path)
        except Exception:
            return False

    @staticmethod
    def _open_port(dev):
        fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0                                            # iflag: raw
        attrs[1] = 0                                            # oflag: raw
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # cflag
        attrs[3] = 0                                            # lflag: raw
        attrs[4] = BAUD
        attrs[5] = BAUD
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        return fd

    def _init_modem(self):
        self.hardware = True
        self._transact("ATE0")          # echo off
        self._transact("AT+CMEE=2")     # verbose errors
        self._transact("AT+CLIP=1")     # caller ID URCs
        self._transact("AT+CVHU=0")     # make hangup commands actually hang up
        self._transact("AT+COPS=3,1")   # short operator names ("T-Mobile", "Tello")
        self._transact("AT+CMGF=1")     # text-mode SMS everywhere
        self._transact("AT+CNMI=2,1,0,0,0")  # push +CMTI URC on new SMS
        final, lines = self._transact("AT+CGSN", timeout=2.0)
        if final == "OK":
            digits = [ln for ln in lines if ln.strip().isdigit()]
            self.imei = digits[0].strip() if digits else None
        print("[MODEM] SIM7600 on %s (IMEI %s). Using REAL modem."
              % (self.port, self.imei or "unknown"))
        # Catch messages that arrived while the phone was off.
        self._events.append(("sms_stored_check", None))
        if not self._allow_calls:
            print("[MODEM] Real call placement DISABLED "
                  "(system.modem.allow_calls=OFF); dial/answer will simulate.")

    # --- locking ---------------------------------------------------------

    def _acquire(self):
        try:
            fcntl.flock(self._lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            return False   # S45modem/atcmd owns the port right now

    def _release(self):
        try:
            fcntl.flock(self._lock_fd, fcntl.LOCK_UN)
        except OSError:
            pass

    # --- AT engine -------------------------------------------------------

    def _read_pending(self):
        """Non-blocking read of whatever the modem sent; yields full lines."""
        try:
            while True:
                chunk = os.read(self.fd, 512)
                if not chunk:
                    break
                self._rxbuf += chunk
        except BlockingIOError:
            pass
        except OSError as exc:
            self._drop_hardware(f"port read failed: {exc}")
            return []
        lines = []
        while b"\n" in self._rxbuf:
            raw, self._rxbuf = self._rxbuf.split(b"\n", 1)
            line = raw.decode("ascii", "replace").strip()
            if line:
                lines.append(line)
        return lines

    def _transact(self, cmd, timeout=2.0):
        """Send cmd, return (final_code, intermediate_lines).

        Caller must hold the lock OR be init-time (probe runs before the
        UI loop, S45modem serializes via the same flock in atcmd). URC
        lines that arrive mid-command are routed to the state machine
        instead of the response. final_code is None on timeout/busy.
        """
        if self.fd is None:
            return None, []
        for line in self._read_pending():   # stale URCs first
            self._handle_urc(line)
        try:
            os.write(self.fd, cmd.encode("ascii") + b"\r")
        except OSError as exc:
            self._drop_hardware(f"port write failed: {exc}")
            return None, []
        deadline = time.monotonic() + timeout
        collected = []
        while time.monotonic() < deadline:
            for line in self._read_pending():
                if line in FINAL_CODES or line.startswith("+CME ERROR") \
                        or line.startswith("+CMS ERROR"):
                    # NO CARRIER etc. mid-call-command doubles as a URC.
                    if line in ("NO CARRIER", "BUSY", "NO ANSWER"):
                        self._handle_urc(line)
                    return line, collected
                if line.startswith(URC_PREFIXES):
                    self._handle_urc(line)
                collected.append(line)
            time.sleep(0.02)
        return None, collected

    def _command(self, cmd, timeout=2.0):
        """Lock-guarded transaction for use from the UI thread."""
        if not self.hardware:
            return None, []
        if not self._acquire():
            return None, []
        try:
            return self._transact(cmd, timeout=timeout)
        finally:
            self._release()

    def _drop_hardware(self, why):
        print(f"[MODEM] Lost the modem ({why}); back to Simulation Mode.")
        try:
            if self.fd is not None:
                os.close(self.fd)
        except Exception:
            pass
        self.fd = None
        self.port = None
        self.hardware = False
        self.state = "IDLE"
        self._csq = None
        self._reg_stat = None
        self.operator = None
        self._rxbuf = b""
        self._events.append(("modem_lost", why))

    # --- URC handling ----------------------------------------------------

    def _handle_urc(self, line):
        # Call-flow URCs go to the console: bring-up needs eyes.
        if line.startswith(("RING", "VOICE CALL:", "NO CARRIER",
                            "MISSED_CALL", "+CLIP:", "BUSY", "NO ANSWER")):
            print(f"[MODEM] {line}")
        if line == "RING":
            if self.state != "RINGING":
                self.state = "RINGING"
                self.caller_id = None
                self._events.append(("incoming", None))
        elif line.startswith("+CLIP:"):
            try:
                number = line.split('"')[1]
            except IndexError:
                number = None
            if number and self.caller_id != number:
                self.caller_id = number
                self._events.append(("incoming", number))
        elif line.startswith("VOICE CALL: BEGIN"):
            self.state = "CONNECTED"
            if self._call_connected_at is None:
                self._call_connected_at = time.monotonic()
            # Some firmwares only accept CPCMREG=1 with a call up;
            # re-assert on the next free tick now that we're connected.
            self._pcm_retry = True
            self._events.append(("connected", self.caller_id))
        elif line.startswith("VOICE CALL: END") or line == "NO CARRIER":
            if self.state != "IDLE":
                self.state = "IDLE"
                self._stop_call_audio()
                self._events.append(("ended", line))
        elif line.startswith("MISSED_CALL:"):
            self.state = "IDLE"
            self._stop_call_audio()
            self._events.append(("missed", line.split(":", 1)[1].strip()))
        elif line.startswith("+CMTI:"):
            # +CMTI: "SM",3 -> a new message landed in slot 3
            try:
                index = int(line.rsplit(",", 1)[1])
            except (IndexError, ValueError):
                return
            self._events.append(("sms_received", index))
        elif line.startswith(("+CEREG:", "+CREG:")):
            self._parse_reg(line)

    def _parse_reg(self, line):
        try:
            fields = line.split(":", 1)[1].split(",")
            # Query form is "<n>,<stat>", unsolicited form is "<stat>[,...]".
            self._reg_stat = int(fields[1] if len(fields) > 1 else fields[0])
        except (IndexError, ValueError):
            pass

    # --- main-loop pump ----------------------------------------------------

    def poll(self):
        """Cheap periodic pump; call every UI tick (rate-limited inside)."""
        now = time.monotonic()

        # Pretend-dial completion: used by Simulation Mode AND by real
        # hardware while system.modem.allow_calls is OFF.
        if self._sim_connect_at and self.state == "CALLING" \
                and now >= self._sim_connect_at:
            self._sim_connect_at = None
            self.state = "CONNECTED"
            if self._call_connected_at is None:
                self._call_connected_at = time.monotonic()
            self._events.append(("connected", None))

        if not self.hardware:
            self._poll_sim(now)
            return

        if now < self._next_urc:
            return
        self._next_urc = now + POLL_URC_S

        if not self._acquire():
            return   # boot script or atcmd session in progress
        try:
            for line in self._read_pending():
                self._handle_urc(line)
            # Deferred CPCMREG=0 (call teardown happened inside a URC
            # handler where the lock was already held).
            if self._pcm_cleanup:
                self._pcm_cleanup = False
                self._transact("AT+CPCMREG=0", timeout=2.0)
            # In-call monitoring: CPCMREG retry, call progress via CLCC,
            # and an aplay watchdog -- bring-up eyes for the console.
            if self.state != "IDLE":
                if self._pcm_retry:
                    self._pcm_retry = False
                    final, _ = self._transact("AT+CPCMREG=1", timeout=3.0)
                    print(f"[MODEM] CPCMREG=1 (in-call retry) -> {final}")
                if now >= self._next_clcc:
                    self._next_clcc = now + CLCC_POLL_S
                    self._poll_clcc()
                self._watch_audio_proc(now)
            # Stagger the queries so one tick never fires more than one.
            if now >= self._next_csq:
                self._next_csq = now + POLL_SIGNAL_S
                final, lines = self._transact("AT+CSQ", timeout=1.5)
                if final == "OK":
                    self._parse_csq(lines)
            elif now >= self._next_net:
                self._next_net = now + POLL_NET_S
                self._transact("AT+CEREG?", timeout=1.5)  # reply parsed as URC
            elif now >= self._next_cops:
                self._next_cops = now + POLL_OPERATOR_S
                final, lines = self._transact("AT+COPS?", timeout=3.0)
                if final == "OK":
                    self._parse_cops(lines)
        finally:
            self._release()

    def _poll_clcc(self):
        """AT+CLCC call-progress check. Caller holds the lock.

        Logs DIALING -> RINGING -> CONNECTED transitions, timestamps the
        connect, and -- crucially for stuck calls -- notices when the
        call list is empty even though we still think a call is up
        (missed VOICE CALL: END), ending the call state cleanly.
        """
        final, lines = self._transact("AT+CLCC", timeout=2.0)
        if final != "OK":
            return
        stat = None
        for line in lines:
            if line.startswith("+CLCC:"):
                try:
                    stat = int(line.split(":", 1)[1].split(",")[2])
                except (IndexError, ValueError):
                    continue
                break
        if stat is None:
            if self.state != "IDLE":
                print("[MODEM] CLCC: no call in the list; ending call state.")
                self.state = "IDLE"
                self._stop_call_audio()
                self._events.append(("ended", "CLCC empty"))
            return
        if stat != self._call_stat:
            self._call_stat = stat
            print(f"[MODEM] Call progress: {CLCC_STATES.get(stat, stat)}")
        if stat == 0:
            if self._call_connected_at is None:
                self._call_connected_at = time.monotonic()
            self.state = "CONNECTED"

    def _watch_audio_proc(self, now):
        """Restart a pipe that died mid-call.

        The speaker retries indefinitely (with holdoff). The mic gets
        three tries, then the call carries on listen-only: no capture
        device is a normal state (e.g. QEMU without a capture-capable
        audio device), not something to spin on.
        """
        port = self._active_pcm_port
        if port is None or now < self._next_audio_restart:
            return
        if self._audio_proc is not None and self._audio_proc.poll() is not None:
            self._next_audio_restart = now + AUDIO_RESTART_HOLDOFF_S
            print(f"[MODEM] Speaker pipe exited rc={self._audio_proc.returncode} "
                  "mid-call; restarting.")
            self._audio_proc = None
            self._start_speaker_pipe(port)
        if self._mic_proc is not None and self._mic_proc.poll() is not None:
            rc = self._mic_proc.returncode
            self._mic_proc = None
            self._mic_fails += 1
            if self._mic_fails >= 3:
                print(f"[MODEM] Mic pipe keeps dying (rc={rc}); giving up -- "
                      "call continues listen-only. Check `arecord -l` and "
                      "the system.hw.modem_mic_device setting.")
            else:
                self._next_audio_restart = now + AUDIO_RESTART_HOLDOFF_S
                print(f"[MODEM] Mic pipe exited rc={rc}; retrying.")
                self._start_mic_pipe(port)

    def call_status(self):
        """(label, connected_seconds|None) for the in-call screen."""
        if self.state == "IDLE":
            return "IDLE", None
        if self._call_stat == 3:
            return "RINGING", None
        if self._call_stat == 2:
            return "CALLING", None
        if self.state == "CONNECTED" or self._call_stat == 0:
            secs = None
            if self._call_connected_at is not None:
                secs = int(time.monotonic() - self._call_connected_at)
            return "CONNECTED", secs
        return self.state, None

    def _parse_csq(self, lines):
        for line in lines:
            if line.startswith("+CSQ:"):
                try:
                    self._csq = int(line.split(":")[1].split(",")[0])
                except (IndexError, ValueError):
                    pass

    def _parse_cops(self, lines):
        for line in lines:
            if line.startswith("+COPS:"):
                self.operator = line.split('"')[1] if '"' in line else None

    def _poll_sim(self, now):
        # Fake incoming call driven by /tmp/neodct_sim_ring. One ring per
        # write/touch (mtime-tracked), so answering or declining doesn't
        # instantly re-ring; `rm` while it rings = the caller gave up.
        if self._exists(SIM_RING_FILE):
            try:
                mtime = os.path.getmtime(SIM_RING_FILE)
            except OSError:
                mtime = None
            if self.state == "IDLE" and mtime != self._sim_ring_mtime:
                self._sim_ring_mtime = mtime
                try:
                    with open(SIM_RING_FILE) as f:
                        self.caller_id = f.read().strip() or "5550000"
                except Exception:
                    self.caller_id = "5550000"
                self.state = "RINGING"
                self._events.append(("incoming", self.caller_id))
        else:
            self._sim_ring_mtime = None
            if self.state == "RINGING":
                self.state = "IDLE"
                self._events.append(("ended", "sim caller gave up"))

        # Fake received SMS driven by /tmp/neodct_sim_sms ("number|text").
        if self._exists(SIM_SMS_FILE):
            content = ""
            try:
                with open(SIM_SMS_FILE) as f:
                    content = f.read().strip()
                os.remove(SIM_SMS_FILE)
            except Exception:
                pass
            if content:
                sender, _, body = content.partition("|")
                if not body:
                    sender, body = "5550000", sender
                self._events.append(("sms_sim", (sender.strip(), body.strip())))

        # Re-probe for late/hotplugged hardware (e.g. modem enumerated
        # after the UI started, or QEMU passthrough attached on the fly).
        if now >= self._next_probe:
            if self._probe_hardware():
                self._events.append(("modem_found", self.port))

    # --- call audio over USB -------------------------------------------------

    def _pcm_port(self):
        """The modem's PCM audio port: USB interface 4 on the SIM7600."""
        try:
            from System.core.SettingsStorage import get_setting
            configured = str(get_setting("system.hw.modem_pcm_port", "AUTO"))
        except Exception:
            configured = "AUTO"
        if configured != "AUTO":
            return configured
        for name in sorted(os.listdir("/sys/class/tty") if os.path.isdir("/sys/class/tty") else []):
            if not name.startswith("ttyUSB"):
                continue
            try:
                with open("/sys/class/tty/%s/device/../bInterfaceNumber" % name) as f:
                    if int(f.read().strip(), 16) == 4:
                        return "/dev/" + name
            except Exception:
                continue
        return "/dev/ttyUSB4"

    @staticmethod
    def _mic_device_setting():
        try:
            from System.core.SettingsStorage import get_setting
            return str(get_setting("system.hw.modem_mic_device", "AUTO")).strip()
        except Exception:
            return "AUTO"

    @staticmethod
    def _find_capture_device():
        """First ALSA device that can actually capture, as plughw:N,M.

        "default" is a trap once the guest has two cards (QEMU's USB
        Audio is playback-only card 0; a passed-through mic lands on
        card 1): ALSA's default maps to card 0 and arecord dies. Scan
        /proc/asound/cardN for pcm*c (c = capture) instead.
        """
        try:
            for entry in sorted(os.listdir(ASOUND_DIR)):
                if not entry.startswith("card") or not entry[4:].isdigit():
                    continue
                for node in sorted(os.listdir(os.path.join(ASOUND_DIR, entry))):
                    if node.startswith("pcm") and node.endswith("c") \
                            and node[3:-1].isdigit():
                        return "plughw:%s,%s" % (entry[4:], node[3:-1])
        except Exception:
            pass
        return None

    def _start_call_audio(self):
        """Full-duplex call audio over the bidirectional PCM port:

            speaker <- aplay   <- PCM port    (far end's voice)
            far end <- PCM port <- arecord    (our USB sound card mic)

        Fire and forget both ways: no mic device means the call is
        listen-only, no PCM port means silent -- everything else works.
        """
        if self._audio_proc is not None or self._mic_proc is not None:
            return
        port = self._pcm_port()
        if not self._exists(port):
            print(f"[MODEM] PCM port {port} not found; call audio unavailable.")
            return
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            attrs = termios.tcgetattr(fd)
            attrs[0] = attrs[1] = attrs[3] = 0   # fully raw: PCM is not text
            attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            os.close(fd)
        except Exception as exc:
            print(f"[MODEM] PCM port setup failed: {exc}")
            return
        self._active_pcm_port = port
        self._mic_fails = 0
        self._start_speaker_pipe(port)
        self._start_mic_pipe(port)

    def _start_speaker_pipe(self, port):
        try:
            self._audio_proc = subprocess.Popen(
                ["aplay", "-q", "-t", "raw", "-f", PCM_FORMAT,
                 "-r", str(self._pcm_rate), "-c", "1", port],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            print(f"[MODEM] Call audio: aplay <- {port} "
                  f"({self._pcm_rate} Hz {PCM_FORMAT}).")
        except Exception as exc:
            self._audio_proc = None
            print(f"[MODEM] Speaker pipe unavailable: {exc}")

    def _start_mic_pipe(self, port):
        device = self._mic_device_setting()
        if device.upper() in ("", "OFF", "NONE"):
            print("[MODEM] Mic uplink disabled (system.hw.modem_mic_device=OFF).")
            return
        if device.upper() == "AUTO":
            device = self._find_capture_device()
            if device is None:
                print("[MODEM] No ALSA capture device found (arecord -l); "
                      "call is listen-only.")
                return
            print(f"[MODEM] Mic auto-detected: {device}")
        try:
            self._mic_proc = subprocess.Popen(
                ["arecord", "-q", "-t", "raw", "-f", PCM_FORMAT,
                 "-r", str(self._pcm_rate), "-c", "1", "-D", device, port],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            print(f"[MODEM] Mic uplink: arecord -D {device} -> {port} "
                  f"({self._pcm_rate} Hz {PCM_FORMAT}).")
        except Exception as exc:
            self._mic_proc = None
            print(f"[MODEM] Mic uplink unavailable ({exc}); call is listen-only.")

    def _stop_call_audio(self):
        """Kill both audio pipes; CPCMREG=0 goes out on the next free tick."""
        stopped = False
        for attr in ("_audio_proc", "_mic_proc"):
            proc = getattr(self, attr)
            if proc is None:
                continue
            try:
                proc.kill()
                proc.wait(timeout=1)
            except Exception:
                pass
            setattr(self, attr, None)
            stopped = True
        if stopped:
            print("[MODEM] Call audio stopped.")
        self._active_pcm_port = None
        self._call_stat = None
        self._call_connected_at = None
        if self.hardware:
            self._pcm_cleanup = True

    # --- call control ------------------------------------------------------

    def dial(self, number):
        number = "".join(c for c in str(number) if c in "0123456789*#+")
        print(f"[MODEM] Requesting Dial: {number}")
        if not number:
            return False
        if not self.hardware or not self._allow_calls:
            if self.hardware:
                print("[MODEM] Calls not enabled yet; simulating this dial.")
            self.state = "CALLING"
            self._sim_connect_at = time.monotonic() + 2.0
            return True
        final, _ = self._command(f"ATD{number};", timeout=8.0)
        if final == "OK":
            self.state = "CALLING"
            self._call_stat = None
            self._call_connected_at = None
            self._next_clcc = 0.0
            # Route audio over USB immediately so ringback/early media
            # is audible while the far end is still ringing. CPCMFRM
            # picks the stream rate (1 = 16 kHz; factory default is 8k).
            frm = "1" if self._pcm_rate == 16000 else "0"
            final_frm, _ = self._command("AT+CPCMFRM=%s" % frm, timeout=2.0)
            final_reg, _ = self._command("AT+CPCMREG=1", timeout=3.0)
            print(f"[MODEM] USB audio setup: CPCMFRM={frm} -> {final_frm}, "
                  f"CPCMREG=1 -> {final_reg}")
            if final_reg != "OK":
                self._pcm_retry = True   # try again once connected
            self._start_call_audio()
            return True
        print(f"[MODEM] Dial failed (final={final})")
        return False

    def answer(self):
        if not self.hardware or not self._allow_calls:
            self.state = "CONNECTED"
            return True
        final, _ = self._command("ATA", timeout=8.0)
        if final == "OK":
            self.state = "CONNECTED"
            self._command("AT+CPCMREG=1", timeout=3.0)
            self._start_call_audio()
            return True
        return False

    def hangup(self):
        print("[MODEM] Requesting Hangup")
        self._sim_connect_at = None
        self._stop_call_audio()
        if not self.hardware:
            self.state = "IDLE"
            return True
        # AT+CHUP is safe with no call up, and it also rejects a live
        # incoming RING even while allow_calls is OFF.
        final, _ = self._command("AT+CHUP", timeout=5.0)
        if final != "OK":
            final, _ = self._command("ATH", timeout=5.0)
        self.state = "IDLE"
        return final == "OK"

    # --- SMS ----------------------------------------------------------------

    def send_sms(self, number, text):
        """Send one text-mode SMS. Returns (ok, detail).

        Real sending (no allow_calls-style gate: SMS *is* the first live
        telephony feature). Simulation Mode pretends and reports so.
        AT+CMGS needs bespoke I/O: the ">" prompt arrives with no newline,
        so the line-based reader in _transact would wait forever.
        """
        number = "".join(c for c in str(number) if c in "0123456789*#+")
        text = str(text).replace("\x1a", "").replace("\x1b", "")
        if not number:
            return False, "no number"
        if not text:
            return False, "empty message"
        print(f"[MODEM] Sending SMS to {number} ({len(text)} chars)")

        if not self.hardware:
            print("[MODEM] (Simulation Mode: pretending the SMS went out.)")
            self._events.append(("sms_sent", number))
            return True, "simulated"

        if not self._acquire():
            return False, "modem port busy"
        try:
            final, _ = self._transact("AT+CMGF=1")   # text mode
            if final != "OK":
                return False, "text mode rejected (%s)" % final
            try:
                os.write(self.fd, b'AT+CMGS="' + number.encode("ascii") + b'"\r')
            except OSError as exc:
                self._drop_hardware(f"port write failed: {exc}")
                return False, "modem lost"
            if not self._wait_sms_prompt(SMS_PROMPT_TIMEOUT_S):
                # ESC backs out of a half-open CMGS so the port isn't
                # stuck eating everything we send next as message body.
                try:
                    os.write(self.fd, b"\x1b")
                except OSError:
                    pass
                return False, "no > prompt from modem"
            try:
                os.write(self.fd, text.encode("ascii", "replace") + b"\x1a")
            except OSError as exc:
                self._drop_hardware(f"port write failed: {exc}")
                return False, "modem lost"

            deadline = time.monotonic() + SMS_SEND_TIMEOUT_S
            ref = None
            while time.monotonic() < deadline:
                for line in self._read_pending():
                    if line.startswith("+CMGS:"):
                        ref = line.split(":", 1)[1].strip()
                    elif line == "OK":
                        print(f"[MODEM] SMS accepted by network (ref {ref}).")
                        self._events.append(("sms_sent", number))
                        return True, ref or "sent"
                    elif line == "ERROR" or line.startswith(("+CMS ERROR", "+CME ERROR")):
                        print(f"[MODEM] SMS rejected: {line}")
                        return False, line
                    elif line.startswith(URC_PREFIXES):
                        self._handle_urc(line)
                time.sleep(0.05)
            return False, "timeout waiting for network"
        finally:
            self._release()

    @staticmethod
    def _parse_sms_records(lines, header):
        """Parse +CMGR:/+CMGL: reply lines into [{index, sender, body}]."""
        records = []
        current = None
        for line in lines:
            if line.startswith(header):
                if current is not None:
                    records.append(current)
                quoted = line.split('"')[1::2]
                sender = quoted[1] if len(quoted) > 1 else "unknown"
                index = None
                if header == "+CMGL:":
                    try:
                        index = int(line.split(":", 1)[1].split(",")[0])
                    except (IndexError, ValueError):
                        pass
                current = {"index": index, "sender": sender, "body": []}
            elif current is not None:
                current["body"].append(line)
        if current is not None:
            records.append(current)
        for rec in records:
            rec["body"] = "\n".join(rec["body"]).strip()
        return records

    def fetch_sms(self, index):
        """Read the +CMTI-announced message, then delete it from the SIM.

        Returns (status, record): status "ok" (record dict), "busy" (port
        locked -- caller should retry later), or "error" (give up).
        """
        if not self.hardware:
            return "error", None
        if not self._acquire():
            return "busy", None
        try:
            self._transact("AT+CMGF=1")
            final, lines = self._transact("AT+CMGR=%d" % index, timeout=5.0)
            if final != "OK":
                print(f"[MODEM] CMGR {index} failed ({final})")
                return "error", None
            records = self._parse_sms_records(lines, "+CMGR:")
            if not records:
                return "error", None
            self._transact("AT+CMGD=%d" % index, timeout=5.0)
            record = records[0]
            record["index"] = index
            print(f"[MODEM] SMS received from {record['sender']} "
                  f"({len(record['body'])} chars)")
            return "ok", record
        finally:
            self._release()

    def read_stored_sms(self):
        """Sweep unread messages waiting on the SIM (arrived while off).

        Returns (status, records) like fetch_sms; each fetched message is
        deleted from the SIM afterward.
        """
        if not self.hardware:
            return "error", []
        if not self._acquire():
            return "busy", []
        try:
            self._transact("AT+CMGF=1")
            final, lines = self._transact('AT+CMGL="REC UNREAD"', timeout=8.0)
            if final != "OK":
                return "error", []
            records = self._parse_sms_records(lines, "+CMGL:")
            for rec in records:
                if rec["index"] is not None:
                    self._transact("AT+CMGD=%d" % rec["index"], timeout=5.0)
            if records:
                print(f"[MODEM] Imported {len(records)} stored SMS from the SIM.")
            return "ok", records
        finally:
            self._release()

    def _wait_sms_prompt(self, timeout):
        """Wait for the raw '>' CMGS prompt (it never gets a newline)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                chunk = os.read(self.fd, 64)
                if chunk:
                    self._rxbuf += chunk
            except BlockingIOError:
                pass
            except OSError:
                return False
            # Complete lines are errors/URCs; the prompt never ends a line.
            while b"\n" in self._rxbuf:
                raw, self._rxbuf = self._rxbuf.split(b"\n", 1)
                line = raw.decode("ascii", "replace").strip()
                if line == "ERROR" or line.startswith(("+CMS ERROR", "+CME ERROR")):
                    return False
                if line.startswith(URC_PREFIXES):
                    self._handle_urc(line)
            if b">" in self._rxbuf:
                self._rxbuf = b""   # swallow the prompt and its trailing space
                return True
            time.sleep(0.02)
        return False

    # --- readouts ----------------------------------------------------------

    def registered(self):
        return self._reg_stat in (1, 5)

    def signal_level(self):
        """0..4 bars for the home-screen sig icon set.

        Simulation Mode returns None (layout keeps its sim_val) unless
        /tmp/neodct_sim_csq supplies a fake rssi.
        """
        if not self.hardware:
            try:
                with open(SIM_CSQ_FILE) as f:
                    return self._bars(int(f.read().strip()))
            except Exception:
                return None
        if self._reg_stat is not None and not self.registered():
            return 0
        return self._bars(self._csq)

    @staticmethod
    def _bars(csq):
        if csq is None or csq == 99:
            return 0
        bars = 0
        for threshold in BAR_THRESHOLDS:
            if csq >= threshold:
                bars += 1
        return bars

    def operator_display(self):
        """Carrier line for the home screen ("Tello", "T-Mobile", ...).

        None means keep the layout's placeholder text ("No Service"):
        not registered, operator not read yet, or Simulation Mode without
        the /tmp/neodct_sim_operator hook.
        """
        if not self.hardware:
            try:
                with open(SIM_OPS_FILE) as f:
                    return f.read().strip() or None
            except Exception:
                return None
        if not self.registered():
            return None
        return self.operator

    def take_pending_event(self):
        """Pop the oldest latched event tuple (kind, detail), or None."""
        return self._events.popleft() if self._events else None

    def requeue_event(self, event):
        """Push an event back to the front (port was busy; retry later)."""
        self._events.appendleft(event)

    def status_snapshot(self):
        """One-look state dump for logs and the future Modem engineering app."""
        return {
            "hardware": self.hardware,
            "port": self.port,
            "imei": self.imei,
            "state": self.state,
            "csq": self._csq,
            "bars": self.signal_level(),
            "reg_stat": self._reg_stat,
            "registered": self.registered(),
            "operator": self.operator,
            "caller_id": self.caller_id,
        }

    def send_at(self, cmd, timeout=5.0):
        """Raw AT passthrough for engineering tools. Returns (final, lines)."""
        return self._command(cmd, timeout=timeout)

    def close(self):
        self._stop_call_audio()
        if self.fd is not None:
            try:
                os.close(self.fd)
            except Exception:
                pass
            self.fd = None
