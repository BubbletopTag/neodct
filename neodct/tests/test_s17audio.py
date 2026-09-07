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

The level is no longer a constant in this script. It is system.hw.mic_gain,
written by MicTest while its owner listens to the effect, and 100 when nothing
has set it. That file is on the writable user partition, so the second half of
these tests is about what the script refuses to pass on from it.

What this file CANNOT cover is the other half of the same fault: a card that
re-enumerates mid-session comes back muted and this script never runs again.
That is nd_modem_audio.c's start_mic_pipe(), which re-applies the mixer before
every call, and neodct/src/test/unit/test_modem.c pins it.
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


def run_start(tmp_path, proc, amixer, settings=None, capture_level=None):
    """Drive `S17audio start`.

    NEODCT_SETTINGS is ALWAYS pointed somewhere under tmp_path, even when the
    test has no settings file to offer: without it the script would fall back
    to the real /NeoDCT/User/settings.prop, and a developer's machine that
    happened to have one would quietly test a different level than CI did.

    NEODCT_CAPTURE_LEVEL is dropped for the same reason and then put back only
    when a test asks for it, which is the one case below that covers the
    override at all. A developer with it exported would otherwise pin every
    level here to their own and pass regardless of what the script parsed.
    """
    conf = tmp_path / "asound.conf"
    env = dict(os.environ,
               NEODCT_ASOUND_CONF=str(conf),
               NEODCT_PROC_ASOUND=str(proc),
               NEODCT_AMIXER=str(amixer),
               NEODCT_SETTINGS=str(settings or (tmp_path / "no-settings.prop")))
    env.pop("NEODCT_CAPTURE_LEVEL", None)
    if capture_level is not None:
        env["NEODCT_CAPTURE_LEVEL"] = str(capture_level)
    result = subprocess.run(["sh", SCRIPT, "start"], capture_output=True,
                            text=True, env=env)
    return result, conf


def settings_file(tmp_path, body):
    path = tmp_path / "settings.prop"
    path.write_text(body)
    return path


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
    """100% with nothing configured, and 100 rather than the 80 this shipped
    with for eight releases. It is the preamp in front of an 8 kHz voice
    codec, not a playback level: every report about this phone's audio has
    been "they cannot hear me"."""
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)

    run_start(tmp_path, proc, amixer)

    assert "cset numid=6 100%" in log.read_text(), log.read_text()


def test_the_owner_can_set_the_level(tmp_path):
    """system.hw.mic_gain is what MicTest writes, and this is the other end of
    it: the level the owner heard themselves choose is the level the card
    comes up at on the next boot."""
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)
    prop = settings_file(tmp_path, "system.ui.wallpaper=NONE\n"
                                   "system.hw.mic_gain=55\n")

    run_start(tmp_path, proc, amixer, settings=prop)

    assert "cset numid=6 55%" in log.read_text(), log.read_text()


def test_a_level_of_zero_is_honoured(tmp_path):
    """0 is a real setting -- it is what a muted card reports, and reproducing
    it deliberately is how you tell "the mixer is not being applied" from "the
    microphone is dead". So it must not be read as "unset"."""
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)
    prop = settings_file(tmp_path, "system.hw.mic_gain=0\n")

    run_start(tmp_path, proc, amixer, settings=prop)

    assert "cset numid=6 0%" in log.read_text(), log.read_text()


def test_a_last_line_with_no_newline_is_still_a_setting(tmp_path):
    """`while read key value` returns non-zero on a final record with no
    newline after it, having already filled the variables -- so the plain form
    silently drops the last line of the file.

    That is not a hypothetical shape. settings.prop is one key per line and
    the last line is whichever key was written last, so the owner's saved mic
    gain was being read as "unset" and the default of 100 applied on every
    boot, with nothing on any screen or in any log to say so. /bin/nd-platform
    guards the same edge on the same file format; this is the other reader,
    and the two must agree about where a file ends."""
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)
    prop = settings_file(tmp_path, "system.ui.wallpaper=NONE\n"
                                   "system.hw.mic_gain=0")  # no trailing \n

    run_start(tmp_path, proc, amixer, settings=prop)

    # 0 and not 100: the value that is easiest to confuse with "nothing was
    # read", which is exactly what the bug did with it.
    assert "cset numid=6 0%" in log.read_text(), log.read_text()


@pytest.mark.parametrize("value", [
    "400",                      # above the ceiling
    "-5",                       # below the floor
    "loud",                     # not a number at all
    "99999999999999999999",     # too long for busybox `test` to compare
    "",                         # present and empty
    "80; touch /tmp/pwned",     # the shape somebody would actually try
    "$(id)",                    # ... and the other shape
])
def test_a_nonsense_level_falls_back_to_the_default(tmp_path, value):
    """settings.prop lives on the writable user partition -- the one place an
    attacker who has got a file onto this phone can write -- and this value
    ends up as an amixer argument. It is read as DATA and never sourced (the
    same rule the initramfs record parser follows), and then checked to be one
    to three digits and no more than 100 before it goes anywhere."""
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)
    prop = settings_file(tmp_path, "system.hw.mic_gain=%s\n" % value)

    result, _ = run_start(tmp_path, proc, amixer, settings=prop)

    assert result.returncode == 0, result.stderr
    calls = log.read_text()
    assert "cset numid=6 100%" in calls, calls
    assert "pwned" not in calls, calls


def test_the_environment_beats_the_settings_file(tmp_path):
    """NEODCT_CAPTURE_LEVEL wins over system.hw.mic_gain, and this is the only
    thing that says so.

    The `: "${NEODCT_CAPTURE_LEVEL:=...}"` default is what makes that true, and
    the script's own comment cited this file as the proof -- while run_start()
    popped the variable out of the environment and no case ever put it back, so
    the override shipped entirely unexercised. It is not decoration: it is how
    a level is pinned on a phone whose settings.prop cannot be written, which
    is every image before /NeoDCT/User has been mounted.
    """
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)
    prop = settings_file(tmp_path, "system.hw.mic_gain=55\n")

    result, _ = run_start(tmp_path, proc, amixer, settings=prop, capture_level=30)

    assert result.returncode == 0, result.stderr
    calls = log.read_text()
    assert "cset numid=6 30%" in calls, calls
    assert "55%" not in calls, calls


def test_a_missing_settings_file_is_not_an_error(tmp_path):
    """First boot: /NeoDCT/User has just been made and holds nothing yet."""
    proc = fake_proc_asound(tmp_path)
    amixer, log = fake_amixer(tmp_path)

    result, _ = run_start(tmp_path, proc, amixer,
                          settings=tmp_path / "never-written.prop")

    assert result.returncode == 0, result.stderr
    assert "cset numid=6 100%" in log.read_text(), log.read_text()


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
