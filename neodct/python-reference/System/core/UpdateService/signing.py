"""RSA PKCS#1 v1.5 / SHA-256 signature verification, no dependencies.

The phone only ever *verifies*, and verification is s^e mod n plus a byte
comparison -- so this needs no crypto library, works identically on the
host test suite and the target, and costs nothing in the image. Releases
are signed on a workstation with:

    openssl dgst -sha256 -sign neodct-release.key -out manifest.sig manifest.json

Deliberately strict: the expected encoded message is rebuilt in full and
compared whole. Verifiers that instead go looking for a DigestInfo inside
the decrypted block are the ones that fall to Bleichenbacher'06 forgeries.
"""

import base64
import hashlib
import hmac
from collections import namedtuple

PublicKey = namedtuple("PublicKey", "n e size")

# DigestInfo(SHA-256) DER prefix, RFC 8017 section 9.2 note 1.
_SHA256_DIGEST_INFO = bytes.fromhex("3031300d060960864801650304020105000420")
_RSA_OID = bytes.fromhex("2a864886f70d010101")
_MIN_PADDING = 8  # RFC 8017: PS must be at least 8 octets


def _der_read(buf, offset=0):
    """Read one DER TLV. Returns (tag, value_bytes, next_offset)."""
    tag = buf[offset]
    length = buf[offset + 1]
    offset += 2
    if length & 0x80:
        count = length & 0x7F
        length = int.from_bytes(buf[offset:offset + count], "big")
        offset += count
    return tag, buf[offset:offset + length], offset + length


def _der_ints(buf):
    """Read a SEQUENCE of INTEGERs (an RSAPublicKey body)."""
    values = []
    offset = 0
    while offset < len(buf):
        tag, value, offset = _der_read(buf, offset)
        if tag != 0x02:
            raise ValueError("expected DER INTEGER, got tag 0x%02x" % tag)
        values.append(int.from_bytes(value, "big"))
    return values


def load_public_key(data):
    """Parse an RSA public key: PEM or DER, SPKI or bare PKCS#1."""
    if isinstance(data, str):
        data = data.encode()
    if b"-----BEGIN" in data:
        body = b"".join(line for line in data.splitlines()
                        if line and not line.startswith(b"-----"))
        data = base64.b64decode(body)

    tag, outer, _ = _der_read(data)
    if tag != 0x30:
        raise ValueError("public key is not a DER SEQUENCE")

    tag, first, next_offset = _der_read(outer)
    if tag == 0x02:
        # PKCS#1 RSAPublicKey: SEQUENCE { INTEGER n, INTEGER e }
        n, e = _der_ints(outer)[:2]
    else:
        # SPKI: SEQUENCE { AlgorithmIdentifier, BIT STRING }
        if _RSA_OID not in first:
            raise ValueError("public key is not an RSA key")
        tag, bitstring, _ = _der_read(outer, next_offset)
        if tag != 0x03:
            raise ValueError("expected a DER BIT STRING")
        _, inner, _ = _der_read(bitstring[1:])  # drop the unused-bits octet
        n, e = _der_ints(inner)[:2]

    return PublicKey(n, e, (n.bit_length() + 7) // 8)


def verify(data, signature, key, algorithm="sha256"):
    """True only if `signature` is a valid signature over `data` by `key`."""
    if algorithm != "sha256":
        raise ValueError("unsupported digest algorithm %r" % algorithm)
    if not signature or len(signature) != key.size:
        return False

    value = int.from_bytes(signature, "big")
    if value >= key.n:
        return False
    encoded = pow(value, key.e, key.n).to_bytes(key.size, "big")

    digest_info = _SHA256_DIGEST_INFO + hashlib.sha256(data).digest()
    padding = key.size - len(digest_info) - 3
    if padding < _MIN_PADDING:
        return False
    expected = b"\x00\x01" + b"\xff" * padding + b"\x00" + digest_info
    return hmac.compare_digest(encoded, expected)


def verify_detached(path, signature_path, key_path):
    """Verify a file against a detached signature. Missing pieces are False."""
    try:
        with open(key_path, "rb") as handle:
            key = load_public_key(handle.read())
        with open(signature_path, "rb") as handle:
            signature = handle.read()
        with open(path, "rb") as handle:
            data = handle.read()
    except (OSError, ValueError):
        return False
    return verify(data, signature, key)
