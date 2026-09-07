"""Every format the phone ships or offers to play must survive the trim.

ffmpeg is built with an explicit codec list to keep libavcodec small, and
it is easy to forget that ffmpeg is not only the video path: mpv is also
what plays ringtones, DTMF, the SMS tone, Koki's audio and MusicPlayer's
fallback. Trimming to the video formats alone silently breaks all of them
-- the phone still boots, the tone list still scrolls, and nothing makes a
sound.

That is exactly what happened once. This test ties the defconfig to the
files actually in the overlay, so the next trim cannot do it again.
"""

import glob
import os

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OVERLAY = os.path.join(ROOT, "overlay")
DEFCONFIGS = sorted(glob.glob(os.path.join(ROOT, "configs", "*_defconfig")))

# Where MusicPlayer states what it will open. It is the C source now: the
# extension lists live in nd_music_is_supported()'s STREAM_EXTS / MPV_EXTS.
MUSIC_EXTS_SOURCE = os.path.join(ROOT, "src", "apps", "MusicPlayer", "meta.c")

# What libavformat/libavcodec each container needs. Audio containers matter
# as much as video ones: 35 .mp3 and 51 .wav files ship in the overlay.
NEEDS = {
    ".mp3":  ("mp3",  ("mp3",)),
    ".wav":  ("wav",  ("pcm_s16le", "pcm_u8")),
    ".ogg":  ("ogg",  ("vorbis",)),
    ".flac": ("flac", ("flac",)),
    ".aac":  ("aac",  ("aac",)),
    ".avi":  ("avi",  ("mjpeg",)),
    ".mp4":  ("mov",  ("h264",)),
}


def _setting(text, key):
    for line in text.splitlines():
        if line.startswith(key + "="):
            return line.split("=", 1)[1].strip().strip('"').split()
    return None


def shipped_extensions():
    """Media extensions of files actually inside the overlay."""
    found = set()
    for path in glob.glob(os.path.join(OVERLAY, "**", "*"), recursive=True):
        ext = os.path.splitext(path)[1].lower()
        if ext in NEEDS and os.path.isfile(path):
            found.add(ext)
    return found


def test_the_overlay_really_does_ship_media():
    # Guard against this whole file quietly passing because the glob broke.
    assert ".mp3" in shipped_extensions()
    assert ".wav" in shipped_extensions()


@pytest.mark.parametrize("defconfig", DEFCONFIGS,
                         ids=[os.path.basename(p) for p in DEFCONFIGS])
def test_every_shipped_format_can_be_demuxed_and_decoded(defconfig):
    text = open(defconfig).read()
    demuxers = _setting(text, "BR2_PACKAGE_FFMPEG_DEMUXERS")
    decoders = _setting(text, "BR2_PACKAGE_FFMPEG_DECODERS")
    if demuxers is None and decoders is None:
        pytest.skip("this defconfig does not trim ffmpeg")

    for ext in sorted(shipped_extensions()):
        demuxer, needed = NEEDS[ext]
        assert demuxer in demuxers, (
            "%s ships %s files but has no %r demuxer; mpv cannot open one"
            % (os.path.basename(defconfig), ext, demuxer))
        assert any(d in decoders for d in needed), (
            "%s ships %s files but none of %r are enabled"
            % (os.path.basename(defconfig), ext, needed))


@pytest.mark.parametrize("defconfig", DEFCONFIGS,
                         ids=[os.path.basename(p) for p in DEFCONFIGS])
def test_music_player_can_play_what_it_advertises(defconfig):
    """MusicPlayer tells the user which files to copy onto the card.

    Its mpv-backed player lists the extensions it accepts, and that list is
    a promise made in the UI. Breaking it means a card full of music the
    phone lists and refuses to play.
    """
    text = open(defconfig).read()
    demuxers = _setting(text, "BR2_PACKAGE_FFMPEG_DEMUXERS")
    if demuxers is None:
        pytest.skip("this defconfig does not trim ffmpeg")

    player = open(MUSIC_EXTS_SOURCE).read()
    advertised = set()
    for line in player.splitlines():
        # STREAM_EXTS[] and MPV_EXTS[] in meta.c, which are the C port of the
        # Python's _MiniaudioPlayer.EXTS / _MpvPlayer.EXTS. The test used to
        # read apps/MusicPlayer/main.py in the overlay; nothing on the phone
        # runs Python any more, so that file stopped existing and this check
        # -- which is the only thing tying the codec trim to the promise the
        # UI makes -- turned into an unconditional FileNotFoundError.
        if "_EXTS[]" not in line:
            continue
        for ext in NEEDS:
            if '"%s"' % ext in line:
                advertised.add(ext)
    assert advertised, "could not read MusicPlayer's EXTS lists from %s" % MUSIC_EXTS_SOURCE

    for ext in sorted(advertised):
        assert NEEDS[ext][0] in demuxers, (
            "MusicPlayer offers %s but %s has no %r demuxer"
            % (ext, os.path.basename(defconfig), NEEDS[ext][0]))
