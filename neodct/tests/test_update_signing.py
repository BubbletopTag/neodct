"""Update signature verification (RSA PKCS#1 v1.5 over SHA-256).

TEST_KEY is a throwaway 2048-bit key generated for this suite only -- it
signs nothing real, and the private exponent is here on purpose so the
tests can forge malformed padding that a real signer would never produce.
SIG_HEX was produced by `openssl dgst -sha256 -sign` over MESSAGE, so the
golden path proves interop with the tool that will sign real releases.
"""

import hashlib
import shutil
import subprocess

import pytest

from System.core.UpdateService import signing

MESSAGE = b"neodct manifest test payload\n"

N_HEX = (
    "f721a3cb4f50022fb8c08b12d7b96efee3a1e56f920bf6679eb9c3272809d751"
    "648223259e0e2f7a2ca752b293818c5d1fa0e69e87a186594ee206c25512c446"
    "c99213be7f7c100c3734cd1b40be82022d9ce20fafb0d84facfa815936638bfe"
    "c2fe83818275ae925bf702df43af28e2178cb18cc92215418995aa728c9fb79c"
    "72348937c92c598a0bacf08c31454568195b15613215f3fade55df70ef19a84f"
    "1568fe6a4f4d229f03efb72f80dd176e82189d4d141e21bf9cf59eda9558ff1a"
    "bcd83cff1c0fc84f0567897cdae2f0332dab6403ec57fe97321ebac58fdf5c7f"
    "4b9dd188b450c229705c972a03a1bbbaef5b063fc3736cab953f7ba970f7ed45"
)
D_HEX = (
    "deb8fa9d68c57ab2f796cfc0139b93653451dc2d493a6ebfc45536843b9962ec"
    "7fe0a93c65cdf30bf0e27bad653304058953c1846e482c84a08b23fc501fb1b4"
    "fa45247632fee4979dc9807067514a6a1c219fbaf364360ed89e8ba49357f3fb"
    "8e5142c39d87e1e515ecf031b7164d8a361f1e84fb603437f47f663606768b4a"
    "36605c623cbf0b5960db8519e0f31ea3f8cffb3c601b9fcaf02a4dd9505a6ecb"
    "612b1be0bbc5ee9c694521f2d91b17db116362b894ea3b5fda87cd2ac6db7e4d"
    "708c1abbcdce22ee3dfafb7cf0629f5732289a432d43e241d34693e7b85b9cd2"
    "69e166616812513724e1dea7c70380fc7366b4da79695a9578ec383017fcd1b"
)
SIG_HEX = (
    "964d58812fc58ae535bd104daf03ab18292dfd8fa08ba8a4797d3b2773075d7c"
    "b736c68aaccbdf8d1223925162c20237759bb2e6024979e3d97bf74b1676ed34"
    "29daf11ad57d3c64eef8904ad8d9c080d553e34997855e323130ed05a2ad7224"
    "3438b2a0aed024cca9eb0a3640d35e12167feac7a458bafe53b7c867bd1429f8"
    "5e45a332f928de84d7dce5e5b15cdd80450f3599dc4b6cf77e0607405c212eee"
    "d5f90426c1eda572bd27d54aeddb2bf88c5235ac77506cce4cafb9d75bd5d41c"
    "ea33fd97cd658bcb7bca1048d301540c0370fe65e26004d967840aab2476e90c"
    "8556180371f7765c7ee4939c016864b2ff38b9921411261e7867d633b3e2f8cc"
)
PUB_PEM = """-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA9yGjy09QAi+4wIsS17lu
/uOh5W+SC/ZnnrnDJygJ11FkgiMlng4veiynUrKTgYxdH6DmnoehhllO4gbCVRLE
RsmSE75/fBAMNzTNG0C+ggItnOIPr7DYT6z6gVk2Y4v+wv6DgYJ1rpJb9wLfQ68o
4heMsYzJIhVBiZWqcoyft5xyNIk3ySxZigus8IwxRUVoGVsVYTIV8/reVd9w7xmo
TxVo/mpPTSKfA++3L4DdF26CGJ1NFB4hv5z1ntqVWP8avNg8/xwPyE8FZ4l82uLw
My2rZAPsV/6XMh66xY/fXH9LndGItFDCKXBclyoDobu671sGP8NzbKuVP3upcPft
RQIDAQAB
-----END PUBLIC KEY-----
"""

SHA256_DIGEST_INFO_PREFIX = bytes.fromhex("3031300d060960864801650304020105000420")
SHA1_DIGEST_INFO_PREFIX = bytes.fromhex("3021300906052b0e03021a05000414")


def raw_sign(em):
    """Sign a pre-encoded message block, bypassing all padding rules."""
    n = int(N_HEX, 16)
    signed = pow(int.from_bytes(em, "big"), int(D_HEX, 16), n)
    return signed.to_bytes((n.bit_length() + 7) // 8, "big")


def test_loads_a_pem_public_key():
    key = signing.load_public_key(PUB_PEM)

    assert key.n == int(N_HEX, 16)
    assert key.e == 65537
    assert key.size == 256


def test_accepts_an_openssl_signature():
    key = signing.load_public_key(PUB_PEM)

    assert signing.verify(MESSAGE, bytes.fromhex(SIG_HEX), key) is True


def test_rejects_a_signature_over_different_data():
    key = signing.load_public_key(PUB_PEM)

    assert signing.verify(MESSAGE + b"!", bytes.fromhex(SIG_HEX), key) is False


def test_rejects_a_signature_of_the_wrong_length():
    key = signing.load_public_key(PUB_PEM)

    assert signing.verify(MESSAGE, bytes.fromhex(SIG_HEX)[:-1], key) is False
    assert signing.verify(MESSAGE, b"\x00" + bytes.fromhex(SIG_HEX), key) is False


def test_rejects_short_padding_even_though_the_digest_is_right():
    """The Bleichenbacher'06 shape: correct DigestInfo, sloppy padding.

    Anything less than a full-width run of 0xff before the DigestInfo has to
    be refused, which is why verification rebuilds the whole expected block
    instead of searching for the digest inside it.
    """
    key = signing.load_public_key(PUB_PEM)
    digest_info = SHA256_DIGEST_INFO_PREFIX + hashlib.sha256(MESSAGE).digest()
    # 0x00 0x01, a too-short 0xff run, 0x00, DigestInfo, then junk padding.
    em = (b"\x00\x01" + b"\xff" * 8 + b"\x00" + digest_info)
    em += b"\x00" * (key.size - len(em))

    assert signing.verify(MESSAGE, raw_sign(em), key) is False


def test_rejects_a_sha1_digest_info():
    """No algorithm substitution: the OID has to be the SHA-256 one."""
    key = signing.load_public_key(PUB_PEM)
    digest_info = SHA1_DIGEST_INFO_PREFIX + hashlib.sha1(MESSAGE).digest()
    padding = b"\xff" * (key.size - len(digest_info) - 3)
    em = b"\x00\x01" + padding + b"\x00" + digest_info

    assert signing.verify(MESSAGE, raw_sign(em), key) is False


def test_rejects_a_garbage_signature():
    key = signing.load_public_key(PUB_PEM)

    assert signing.verify(MESSAGE, b"\x2a" * 256, key) is False


def test_rejects_an_empty_signature():
    key = signing.load_public_key(PUB_PEM)

    assert signing.verify(MESSAGE, b"", key) is False


def test_accepts_the_reference_encoding_built_by_hand():
    """Sanity check on raw_sign itself: a correctly padded block verifies."""
    key = signing.load_public_key(PUB_PEM)
    digest_info = SHA256_DIGEST_INFO_PREFIX + hashlib.sha256(MESSAGE).digest()
    padding = b"\xff" * (key.size - len(digest_info) - 3)
    em = b"\x00\x01" + padding + b"\x00" + digest_info

    assert signing.verify(MESSAGE, raw_sign(em), key) is True


@pytest.mark.skipif(shutil.which("openssl") is None, reason="openssl not installed")
def test_interoperates_with_freshly_generated_openssl_keys(tmp_path):
    """A real 4096-bit release-shaped key, signed and verified end to end."""
    key_path = tmp_path / "release.key"
    pub_path = tmp_path / "release.pub"
    payload = tmp_path / "manifest.json"
    sig_path = tmp_path / "manifest.sig"
    payload.write_bytes(b'{"version": "0.3.2a"}')
    subprocess.run(["openssl", "genpkey", "-algorithm", "RSA",
                    "-pkeyopt", "rsa_keygen_bits:4096", "-out", str(key_path)],
                   check=True, capture_output=True)
    subprocess.run(["openssl", "rsa", "-in", str(key_path), "-pubout",
                    "-out", str(pub_path)], check=True, capture_output=True)
    subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(key_path),
                    "-out", str(sig_path), str(payload)], check=True)

    key = signing.load_public_key(pub_path.read_bytes())

    assert key.size == 512
    assert signing.verify(payload.read_bytes(), sig_path.read_bytes(), key) is True
    assert signing.verify(b"other", sig_path.read_bytes(), key) is False
