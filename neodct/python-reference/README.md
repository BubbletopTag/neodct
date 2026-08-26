# The Python NeoDCT, kept as the specification

This is `overlay/NeoDCT/` as it was before the C rewrite: the phone OS that
every file under `neodct/src/` is a port of. It is here and not in the
overlay because **it no longer runs, and it must not ship**.

Nothing in this directory is built, installed, or read at runtime. The
overlay is `BR2_ROOTFS_OVERLAY`, so anything left in it lands on the phone
verbatim; 1.6 MB of source for a program that has no interpreter to run it
is dead weight in a 53 MB image, and a `main.py` sitting beside an `app.so`
invites the reader to think it is the thing being executed. It is not --
`/bin/run_neodct.sh` execs `nd-core`, and `nd-apprun` `dlopen()`s
`<app-dir>/app.so`. The `"exec": "main.py"` field still in every
`manifest.json` is parsed and stored by the app registry and then ignored,
which is the Python's own behaviour and is pinned by
`test/unit/test_appreg.c`.

## Why it is kept at all

Because it is the specification, and the port cites it constantly. Open
almost any C file and the header says what it is a port of and at which
line:

    /* nd_predictive.c -- PredictiveText, ported from System/ui/framework.py:93. */

Those citations resolve here. The paths are unchanged below this directory,
so `System/ui/framework.py` in a comment is
`neodct/python-reference/System/ui/framework.py` on disk. When a C
behaviour looks wrong, this is the file that settles it -- including the
bugs, which were ported deliberately and are marked as such at their sites.

`docs/c-rewrite/spec-*.md` is the prose form of the same thing, and the two
disagree occasionally. Where they do, this directory wins: it is what the
phone actually did.
