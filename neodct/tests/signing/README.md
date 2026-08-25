# Signature fixtures

Cross-verification material for `lib/nd_signing.c`. Every verdict here was
first established by running the REAL Python — `System/core/UpdateService/
signing.py` — and `test_signing.c` asserts the C agrees with it:

| fixture | Python's verdict |
| --- | --- |
| `data.bin` + `data.sig` + `k.pub` | **True** |
| `tampered.bin` + `data.sig` + `k.pub` | False |
| `data.bin` + `data.sig` + `other.pub` | False |
| `data.sig` truncated by one byte | False |
| an empty signature | False |

`k.pub` is a 2048-bit RSA public key; `data.sig` is
`openssl dgst -sha256 -sign` over `data.bin`. `tampered.bin` differs from
`data.bin` in one character ("manifest" -> "manifezt"), so the signature is
intact and only the message moved.

**The private keys are deliberately not here.** They were generated in /tmp,
used once to produce `data.sig`, and never committed — a test that needs a
signing key is a test that has put a signing key in a repository. To add a
case, generate a fresh pair in a temporary directory, sign, and commit only
the public half and the signature.

The phone's real key is `overlay/NeoDCT/System/keys/neodct-release.pub` and
its private half has never been in this repository at all.
