"""Shared test material for the update suites.

The key here is a throwaway 2048-bit RSA key generated for the test suite
only. Its private exponent is included deliberately so tests can sign real
packages -- and can forge malformed ones that a proper signer would never
emit. It is not the release key and never signs anything shipped.
"""

import hashlib
import json
import zipfile

from System.core.UpdateService import verity as verity_mod

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
E = 65537
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
BS = 4096
PLATFORM = "qemu-aarch64"


def raw_sign(em):
    """Sign a pre-encoded message block, bypassing all padding rules."""
    n = int(N_HEX, 16)
    signed = pow(int.from_bytes(em, "big"), int(D_HEX, 16), n)
    return signed.to_bytes((n.bit_length() + 7) // 8, "big")


def sign(data):
    """A correct PKCS#1 v1.5 / SHA-256 signature over `data`."""
    size = (int(N_HEX, 16).bit_length() + 7) // 8
    digest_info = SHA256_DIGEST_INFO_PREFIX + hashlib.sha256(data).digest()
    padding = b"\xff" * (size - len(digest_info) - 3)
    return raw_sign(b"\x00\x01" + padding + b"\x00" + digest_info)


def pattern(nbytes, seed=b"neodct"):
    """Deterministic, non-trivial test data."""
    out = bytearray()
    h = seed
    while len(out) < nbytes:
        h = hashlib.sha256(h).digest()
        out += h
    return bytes(out[:nbytes])


def write_public_key(tmp_path, name="release.pub"):
    path = tmp_path / name
    path.write_text(PUB_PEM)
    return path


def build_image(blocks=8, seed=b"neodct"):
    """A stand-in system image: `blocks` of data with a verity tree appended.

    Nothing here parses squashfs, so random-looking blocks are as good as a
    real filesystem for exercising hashing, staging and verity geometry.
    """
    data = pattern(blocks * BS, seed=seed)
    tree = verity_mod.build_hash_tree(data, salt=bytes.fromhex("f8e7d6c5b4a3"))
    return data + verity_mod.format_hash_area(tree), tree


def png(size=64, colour=(180, 200, 255)):
    """A real PNG, so the phone-side image loader has something to open."""
    import io

    from PIL import Image, ImageDraw

    image = Image.new("RGB", (size, size), colour)
    draw = ImageDraw.Draw(image)
    draw.ellipse((size * 0.15, size * 0.15, size * 0.85, size * 0.85),
                 outline="black", width=max(1, size // 16))
    buffer = io.BytesIO()
    image.save(buffer, "PNG")
    return buffer.getvalue()


def make_ndsw(path, image=None, tree=None, manifest_body=None, signature=True,
              members=("rootfs.squashfs", "manifest.json", "manifest.sig"),
              thumbnail=None, thumbnail_hash=True,
              **manifest_overrides):
    """Write a real .ndsw zip. Returns the manifest dict that went in.

    thumbnail       bytes to store as thumbnail.png (None: no such member)
    thumbnail_hash  True records the real hash; pass bytes/str to record a
                    different one, or False to record none at all.
    """
    if image is None:
        image, tree = build_image()

    body = {
        "version": "0.3.2a",
        "buildtime": 1785160800,
        "changelog": "Fixed SMS database sorting bug.",
        "platform": PLATFORM,
        "sha256": hashlib.sha256(image).hexdigest(),
        "verity": {
            "root_hash": tree.root_hash,
            "block_size": tree.data_block_size,
            "image_blocks": tree.data_blocks,
            "salt": tree.salt.hex(),
        },
    }
    if thumbnail is not None and thumbnail_hash is not False:
        body["thumbnail_sha256"] = (
            hashlib.sha256(thumbnail).hexdigest() if thumbnail_hash is True
            else thumbnail_hash)
    if manifest_body is not None:
        body = manifest_body
    body.update(manifest_overrides)
    raw = json.dumps(body, indent=2).encode()

    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        if "rootfs.squashfs" in members:
            zf.writestr("rootfs.squashfs", image)
        if thumbnail is not None:
            zf.writestr("thumbnail.png", thumbnail)
        if "manifest.json" in members:
            zf.writestr("manifest.json", raw)
        if "manifest.sig" in members:
            zf.writestr("manifest.sig",
                        sign(raw) if signature is True else signature)
    return body
