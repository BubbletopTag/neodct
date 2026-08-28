"""S17audio has to leave the microphone able to actually record.

It has always done the routing: find the USB card by usbid, write
/etc/asound.conf pointing "default" at it, bypass dmix. Then it stops.

It never touches the mixer, and nothing else does either -- there is no
alsactl in the image, so no saved state is restored at boot. Whatever the
driver defaults to is what the phone gets, on every boot and every replug.

For the ONN microphone that was fine; its defaults are open. For an electret
soldered to a C-Media card it is not: the capture switch comes up off and the
mic gain at zero, and arecord then returns a flat line of silence with no
error at all. MicTest draws that faithfully, which looks exactly like a dead
microphone.
"""

import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(REPO, "overlay", "etc", "init.d", "S17audio")

# What a C-Media card really offers: a playback control, a capture control
# with both a volume and a switch, and the AGC switch.
# The control list a REAL C-Media adapter (0d8c:0014, Unitek Y-247A) publishes,
# read off the hardware via /proc/asound/cardN/usbmixer. The shape matters:
# "Mic Capture Volume" and "Mic Playback Volume" are separate kernel controls
# that ALSA's simple mixer merges into one control called "Mic" carrying BOTH
# pvolume and cvolume -- so `amixer sset Mic 80%` raises the monitor path that
# feeds the microphone back into the speaker. On a phone that is feedback, and
# a test whose fake mixer has no such control would never notice.
CONTROLS = """numid=1,iface=MIXER,name='Speaker Playback Switch'
numid=2,iface=MIXER,name='Speaker Playback Volume'
numid=3,iface=MIXER,name='Mic Playback Switch'
numid=4,iface=MIXER,name='Mic Playback Volume'
numid=5,iface=MIXER,name='Mic Capture Switch'
numid=6,iface=MIXER,name='Mic Capture Volume'
numid=7,iface=MIXER,name='Auto Gain Control'
"""


def fake_amixer(tmp_path):
    """Records every call and answers `controls` like the real card."""
    log = tmp_path / "amixer.log"
    tool = tmp_path / "amixer"
    tool.write_text(
        "#!/bin/sh\n"
        'echo "$*" >> "@LOG@"\n'
        'if [ "$1" = "-c" ]; then shift 2; fi\n'
        'case "$1" in\n'
        '  controls) cat <<EOF\n@CONTROLS@\nEOF\n'
        "    ;;\n"
        "esac\n"
        "exit 0\n".replace("@LOG@", str(log)).replace("@CONTROLS@", CONTROLS.strip())
    )
    tool.chmod(0o755)
    return tool, log


def fake_proc_asound(tmp_path, card=1, usb=True):
    root = tmp_path / "proc-asound"
    (root / ("card%d" % card)).mkdir(parents=True)
    if usb:
        (root / ("card%d" % card) / "usbid").write_text("0d8c:0014\n")
    (root / "cards").write_text(
        " 0 [rvadcodec     ]: simple-card - rv-adcodec\n"
        "                    rv-adcodec\n"
        " %d [Device        ]: USB-Audio - USB PnP Sound Device\n"
        "                    C-Media USB PnP Sound Device\n" % card
    )
    return root


def run_start(tmp_path, proc, amixer):
    conf = tmp_path / "asound.conf"
    env = dict(os.environ,
               NEODCT_ASOUND_CONF=str(conf),
               NEODCT_PROC_ASOUND=str(proc),
               NEODCT_AMIXER=str(amixer))
    result = subprocess.run(["sh", SCRIPT, "start"], capture_output=True,
                            text=True, env=env)
    return result, conf


@pytest.mark.skipif(shutil.which("sh") is None, reason="no shell")
def test_the_capture_switch_is_turned_on(tmp_path):
    """Without this the electret records silence and nothing says why."""
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)

    result, _ = run_start(tmp_path, proc, amixer)

    assert result.returncode == 0, result.stderr
    assert log.exists(), "S17audio never ran amixer at all"
    assert "cset numid=5 on" in log.read_text(), log.read_text()


def test_the_capture_volume_is_raised(tmp_path):
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)

    run_start(tmp_path, proc, amixer)

    assert "cset numid=6 80%" in log.read_text(), log.read_text()


def test_the_microphone_monitor_path_is_left_alone(tmp_path):
    """numid 3 and 4 are Mic PLAYBACK -- the mic fed back into the speaker.

    This is the case the first version of this fix got wrong: addressing the
    simple-mixer control "Mic" sets playback and capture together, because the
    two are merged under one name. Feedback in the earpiece is a worse bug than
    the silence being fixed.
    """
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)

    run_start(tmp_path, proc, amixer)

    calls = log.read_text()
    assert "cset numid=3" not in calls, "touched Mic Playback Switch: %s" % calls
    assert "cset numid=4" not in calls, "touched Mic Playback Volume: %s" % calls


def test_speaker_volume_is_left_alone(tmp_path):
    """The Music app owns playback volume; do not fight it at every boot."""
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)

    run_start(tmp_path, proc, amixer)

    calls = log.read_text()
    assert "cset numid=1" not in calls, calls
    assert "cset numid=2" not in calls, calls


def test_auto_gain_control_is_not_forced_on(tmp_path):
    """A sound-quality choice, not a correctness one. Left to the owner."""
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)

    run_start(tmp_path, proc, amixer)

    assert "cset numid=7" not in log.read_text(), log.read_text()


def test_the_routing_still_happens(tmp_path):
    """The mixer work must not have broken what the script already did."""
    proc = fake_proc_asound(tmp_path)
    amixer, _ = fake_amixer(tmp_path)

    _, conf = run_start(tmp_path, proc, amixer)

    text = conf.read_text()
    assert "card 1" in text
    assert "type plug" in text


def test_no_usb_card_is_not_an_error(tmp_path):
    """A phone with the sound card unplugged still boots."""
    proc = fake_proc_asound(tmp_path, usb=False)
    amixer, _ = fake_amixer(tmp_path)

    result, conf = run_start(tmp_path, proc, amixer)

    assert result.returncode == 0, result.stderr
    assert not conf.exists()
