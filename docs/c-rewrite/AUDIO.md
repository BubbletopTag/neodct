# Audio, mpv, and whether MPlayer would help

The project owner asked whether replacing mpv with "MPlayer, vo fbdev" would be
better on memory. Short answer: **MPlayer is not the fix, and it is not
available anyway — but the underlying instinct is right, and 0.4.0 already acts
on it.**

---

## Why mpv is expensive

mpv's Buildroot package pulls in **ffmpeg, swscale, libass** and requires
**libstdc++**. That is a full media stack to play a ringtone.

The project already measured the consequence, in `docs/KOKI_PORT_NOTES.md`:

> mpv memory reality check (measured): demuxer/cache flags do NOT shrink it
> (our tracks are <1MiB whole files); **~24MB private per process** on a desktop
> build is codec/core init.

24 MB, per process, on a phone with ~53 MB usable. That is the whole problem,
and it is not a tuning problem — the flags were tried and do nothing, because
the memory goes on initialising codecs before a single sample is decoded.

---

## Why MPlayer is not the answer

**It is not in this Buildroot.** Buildroot 2025.11 ships `alsa-utils`,
`madplay`, `mpg123` and `mpv`. There is no `mplayer` package. Using it would
mean writing and maintaining one.

And it would very likely not pay. MPlayer links ffmpeg's decoders too — that is
where the memory goes in mpv, and MPlayer does not avoid it by having an older
UI. `vo fbdev` addresses *video output*, which this phone does not use for a
ringtone at all; the cost being measured is audio codec initialisation. Swapping
one ffmpeg front-end for another ffmpeg front-end is not a saving, it is a
rewrite that ends up in the same place.

---

## What 0.4.0 does instead

Stop launching a media player.

The ringtone decoder is now **in-process**, using vendored `dr_mp3.h` and
`dr_wav.h` — two single-header decoders, no ffmpeg, no libstdc++, no separate
process. It streams: a few thousand source frames at a time into a small ring
buffer, rather than the Python's `miniaudio.decode_file()` which decodes the
whole tone into memory first. Risk R-9 measured that at up to **6.3 MB for one
shipped ringtone**.

PCM then goes to **`aplay`**, and that choice is worth explaining because it
deviates from the Python:

- `aplay` is **already a hard dependency** — both defconfigs set
  `BR2_PACKAGE_ALSA_UTILS_APLAY=y`. It adds nothing to the image.
- It costs a few hundred kB of RSS against mpv's ~24 MB.
- The Python opens a `miniaudio.PlaybackDevice`, which is miniaudio's ALSA
  backend. Writing to `aplay` reaches the same ALSA device by a different road.

The Python's docstring says miniaudio exists specifically to avoid the ~24 MB
mpv process. 0.4.0 honours **that reason** rather than the letter of the
implementation.

**mpv remains the fallback**, reached by exactly the condition that reaches it
in the Python: the decode failed. `dr_mp3` and `dr_wav` cannot read `.wma`,
`.flac` or `.ogg` — and neither can miniaudio, so a `.wma` tone already falls
through to mpv today and still does. Nothing regressed; the common path just
stopped being expensive.

---

## The measured picture

| Path | Cost | When |
| --- | --- | --- |
| in-process `dr_mp3`/`dr_wav` → `aplay` | a few hundred kB | mp3, wav — the common case |
| `mpv` | ~24 MB private | wma, flac, ogg — fallback only |
| `mpg123` | small, mp3 only | available, currently unused |

---

## If the ringtone path ever needs to get smaller again

In rough order of value:

1. **Cover the fallback formats in-process.** `.wma` is the awkward one; `.ogg`
   and `.flac` both have small single-header or single-library decoders
   (`stb_vorbis`, `dr_flac`) that would fit the same streaming shape. Cover
   those three and mpv can leave the image entirely — which also drops ffmpeg,
   swscale, libass and libstdc++ from the rootfs, a far larger saving than the
   RSS figure suggests.
2. **Then drop `BR2_PACKAGE_MPV`.** Only after step 1, and check nothing else
   selects it first.
3. `mpg123` is already in the image and could replace mpv for mp3 specifically,
   but the in-process decoder already covers mp3, so this buys nothing today.

**What is not worth doing:** adding an MPlayer package. It is not in Buildroot,
it carries the same ffmpeg weight, and the format gap it would close is closed
better by two more single-header decoders.

---

## Not yet verified on musl

Neither mpv nor NetSurf has been built against musl. The environment this was
written in blocks Buildroot's downloads (403 on `sources.buildroot.net` and
friends), so no cross-toolchain exists here to try it.

Both are ordinary C/C++ that Buildroot builds against musl routinely, so no
problem is expected — but expected is not verified, and the first musl image
should be watched for exactly this. `docs/c-rewrite/MUSL.md` lists the
behavioural differences that could plausibly bite: the much smaller default
thread stack, and the simpler resolver.
