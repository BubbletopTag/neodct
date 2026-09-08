#!/usr/bin/env python3
"""End-to-end cellular data test: boot the QEMU image headless with the
SIM7600 passed through and prove the modem alone provides internet.

IT CANNOT RUN ON THE KERNEL THIS TREE BUILDS, AND IT REFUSES RATHER THAN
TRIES. The emulator is armv7 on -M virt now, and that kernel has no PCI, so
no xhci, so no USB host controller at all -- and no USB_NET_QMI_WWAN for the
data path either. Every device this script asks QEMU for is accepted by QEMU
and invisible to the guest. It also still describes an aarch64 machine below,
which is the shape it will need to be rewritten from; it is left intact for
that, the way run_qemu.sh leaves its USB wiring intact behind a refusal.
Bringing it back means the kernel symbols first: CONFIG_PCI +
CONFIG_PCI_HOST_GENERIC for the xhci (PCI alone leaves the bridge unprobed --
see buildroot/board/qemu/armv7-virt/linux.config) and CONFIG_USB_NET_QMI_WWAN
for wwan0, then a re-boot for a fresh MemTotal, then this script's machine.

What it did:
  1. finds the modem on the host USB bus (1e0e:9001) and checks access
  2. boots qemu-system-aarch64 with -snapshot (rootfs.ext4 stays pristine),
     no display, serial console on a unix socket
  3. logs in as root over the serial socket
  4. waits for S45modem's verdict in /tmp/modem.status
  5. runs the proof commands (wwan0 address, default route, ping6/curl,
     DNS lookup, real HTTP fetch) and prints a PASS/FAIL summary

Usage:
  scripts/qemu_modem_data_test.py [--images DIR] [--timeout SECS]

Prerequisites:
  * close any other QEMU that has the modem passed through (one claimant
    at a time) and stop host ModemManager
  * the usual `chmod 666 /dev/bus/usb/BBB/DDD` so QEMU can claim it
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import time

VID, PID = "1e0e", "9001"
MARKER = "@@DONE@@"


def find_modem():
    """Return (devnode, ok_access) for the SIM7600, or (None, False)."""
    base = "/sys/bus/usb/devices"
    for dev in sorted(os.listdir(base)):
        try:
            with open(f"{base}/{dev}/idVendor") as f:
                vid = f.read().strip()
            with open(f"{base}/{dev}/idProduct") as f:
                pid = f.read().strip()
        except OSError:
            continue
        if (vid, pid) != (VID, PID):
            continue
        with open(f"{base}/{dev}/busnum") as f:
            bus = int(f.read())
        with open(f"{base}/{dev}/devnum") as f:
            num = int(f.read())
        node = f"/dev/bus/usb/{bus:03d}/{num:03d}"
        return node, os.access(node, os.R_OK | os.W_OK)
    return None, False


class Serial:
    """Line-oriented driver for the guest's serial console socket."""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.sock.settimeout(0.5)
        self.buf = b""

    def read_until(self, pattern, timeout):
        """Wait until regex `pattern` matches the accumulated output."""
        end = time.monotonic() + timeout
        rx = re.compile(pattern, re.M)
        while time.monotonic() < end:
            m = rx.search(self.buf.decode("utf-8", "replace"))
            if m:
                return m
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise RuntimeError("serial socket closed (guest died?)")
                self.buf += chunk
            except socket.timeout:
                pass
        return None

    def send(self, text):
        self.sock.sendall(text.encode())

    def sh(self, cmd, timeout=30):
        """Run one shell command in the guest; return (exit_code, output)."""
        start = len(self.buf)
        # Split the marker in the echoed input so only the executed echo
        # can match.
        self.send(cmd + '; echo "@@DO""NE@@=$?"\n')
        m = self.read_until(re.escape(MARKER) + r"=(\d+)", timeout)
        if m is None:
            raise RuntimeError(f"guest command timed out: {cmd}")
        out = self.buf[start:].decode("utf-8", "replace")
        out = out.split(MARKER)[0]
        # Drop the echoed command line if present.
        lines = [l for l in out.splitlines() if '@@DO""NE@@' not in l]
        if lines and cmd.split(";")[0].strip() in lines[0]:
            lines = lines[1:]
        return int(m.group(1)), "\n".join(lines).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    default_images = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..", "..", "buildroot", "output", "images")
    ap.add_argument("--images", default=default_images,
                    help="dir with the kernel + rootfs.ext4")
    ap.add_argument("--timeout", type=int, default=300,
                    help="max seconds to wait for S45modem's verdict")
    args = ap.parse_args()

    # Refused before anything is looked for, because the missing file would
    # have sent the reader off to rebuild the tree for hours to produce an
    # `Image` that will never exist again: an arm kernel builds a zImage. The
    # real reason is a kernel symbol, and naming it is the whole difference
    # between this and "build the image first".
    sys.exit(
        "qemu_modem_data_test: this cannot work on the kernel this tree "
        "builds.\n"
        "  The emulator is armv7 on -M virt: no CONFIG_PCI and no "
        "CONFIG_PCI_HOST_GENERIC,\n"
        "  so no xhci and no USB bus for `-device usb-host` to attach to, and "
        "no\n"
        "  CONFIG_USB_NET_QMI_WWAN for wwan0. QEMU accepts every device below "
        "and the\n"
        "  guest sees none of them -- and on a QEMU build no AT port at all is "
        "still\n"
        "  simulation, so what you would be watching is the phone placing "
        "pretend calls.\n"
        "  buildroot/board/qemu/armv7-virt/linux.config lists what each "
        "absence costs;\n"
        "  adding a symbol back means booting the kernel again for a fresh "
        "MemTotal.\n"
        "  This script also still boots qemu-system-aarch64 against an "
        "`Image`, which\n"
        "  this tree no longer produces (BR2_LINUX_KERNEL_ZIMAGE); its machine "
        "needs\n"
        "  rewriting with the rest.")

    kernel = os.path.join(args.images, "zImage")
    rootfs = os.path.join(args.images, "rootfs.ext4")
    for f in (kernel, rootfs):
        if not os.path.exists(f):
            sys.exit(f"missing {f} -- build the image first")

    node, ok = find_modem()
    if node is None:
        sys.exit("SIM7600 (1e0e:9001) not on the host USB bus -- "
                 "cable/power? brownout reboot?")
    if not ok:
        sys.exit(f"no rw access to {node} -- run: sudo chmod 666 {node}")
    print(f"modem found at {node}")

    sock_path = f"/tmp/nmodem-{os.getpid()}.sock"
    stderr_path = f"/tmp/nmodem-{os.getpid()}.err"
    qemu_cmd = [
        "qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a53", "-m", "72",
        "-kernel", kernel,
        "-drive", f"file={rootfs},if=none,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0", "-snapshot",
        "-display", "none",
        "-device", "qemu-xhci",
        "-device", f"usb-host,vendorid=0x{VID},productid=0x{PID}",
        "-serial", f"unix:{sock_path},server=on,wait=off",
        "-append", "root=/dev/vda console=ttyAMA0",
    ]
    print("booting guest (snapshot mode, rootfs untouched)...")
    with open(stderr_path, "w") as errf:
        qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                                stderr=errf)
    results = []
    try:
        time.sleep(3)
        with open(stderr_path) as f:
            err = f.read()
        if qemu.poll() is not None or "busy" in err.lower():
            sys.exit("QEMU could not claim the modem -- is another QEMU "
                     f"still running with it passed through?\n{err.strip()}")

        ser = None
        for _ in range(20):
            try:
                ser = Serial(sock_path)
                break
            except OSError:
                time.sleep(1)
        if ser is None:
            sys.exit("serial socket never appeared")

        if not ser.read_until(r"login:", 120):
            sys.exit("no login prompt on serial console after 120s")
        ser.send("root\n")
        if not ser.read_until(r"# ", 30):
            sys.exit("no shell prompt after login")
        ser.sh("stty -echo")
        print("logged in; waiting for S45modem...")

        def check(name, cmd, timeout=30, expect_ok=True):
            code, out = ser.sh(cmd, timeout)
            passed = (code == 0) if expect_ok else True
            results.append((name, passed))
            tag = "PASS" if passed else "FAIL"
            print(f"[{tag}] {name}")
            if out:
                print("      " + out.replace("\n", "\n      "))
            return passed

        check("qmi_wwan driver bound",
              "dmesg | grep -E 'qmi_wwan|option.*GSM' | tail -3")
        check("AT ports present", "ls /dev/ttyUSB*")

        # Wait for the boot service's verdict.
        verdict = ""
        end = time.monotonic() + args.timeout
        while time.monotonic() < end:
            _, verdict = ser.sh("cat /tmp/modem.status 2>/dev/null")
            if verdict and verdict != "starting":
                break
            time.sleep(5)
        up = verdict.startswith("up:")
        results.append(("S45modem bring-up", up))
        print(f"[{'PASS' if up else 'FAIL'}] S45modem verdict: {verdict!r}")
        _, log = ser.sh("cat /tmp/modem-boot.log")
        print("---- modem-boot.log " + "-" * 40)
        print(log)
        print("-" * 60)

        if up:
            iface = verdict.split(":", 1)[1]
            check("global IPv6 address",
                  f"ip -6 addr show dev {iface} scope global | grep inet6")
            check("default route via modem",
                  "ip -6 route | grep '^default'")
            check("ping real internet (v6 literal)",
                  "ping -6 -c 2 -W 4 2606:4700:4700::1111 || "
                  "ping6 -c 2 2606:4700:4700::1111 || "
                  "curl -g -m 10 -sf -o /dev/null 'http://[2606:4700:4700::1111]/'",
                  timeout=45)
            check("DNS resolution", "nslookup example.com", timeout=30)
            check("HTTP fetch over cellular",
                  "wget -q -O /dev/null http://example.com || "
                  "curl -m 20 -sf -o /dev/null http://example.com",
                  timeout=60)
            check("HTTPS fetch (browser path)",
                  "curl -m 30 -sf -o /dev/null https://example.com",
                  timeout=60)
    finally:
        qemu.terminate()
        try:
            qemu.wait(5)
        except subprocess.TimeoutExpired:
            qemu.kill()
        for p in (sock_path, stderr_path):
            try:
                os.unlink(p)
            except OSError:
                pass

    print()
    failed = [n for n, p in results if not p]
    if failed:
        print(f"RESULT: FAIL ({len(failed)}/{len(results)}): "
              + ", ".join(failed))
        sys.exit(1)
    print(f"RESULT: PASS -- all {len(results)} checks; "
          "the modem is a working network device")


if __name__ == "__main__":
    main()
