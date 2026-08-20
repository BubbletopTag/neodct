"""dm-verity hash tree construction.

Layout matches cryptsetup/veritysetup (hash type 1), which is what the
kernel's dm-verity target expects:

  * every digest is sha256(salt || block)
  * level 0 holds the digests of the data blocks, packed contiguously into
    hash-block-sized blocks and zero padded
  * each level above holds the digests of the level below it
  * levels are stored top level FIRST, level 0 LAST (the kernel walks them
    in that order -- see verity_ctr()'s hash_level_block[] loop)
  * the root hash is the digest of the single top block; with only one data
    block there is no tree at all and the root is that block's own digest
"""

import hashlib
import struct
import uuid as _uuid
from collections import namedtuple

# A tree is meaningless without the salt and geometry it was built with, so
# it carries them: format_hash_area() and dm_table() must never be handed a
# salt that disagrees with the digests.
HashTree = namedtuple(
    "HashTree",
    "root_hash tree levels data_blocks hash_blocks "
    "salt data_block_size hash_block_size algorithm",
)

SUPERBLOCK_MAGIC = b"verity\x00\x00"
SUPERBLOCK_SIZE = 512
HASH_TYPE = 1  # 0 = Chrome OS, 1 = normal
_SB_STRUCT = "<8sII16s32sIIQH6s256s168s"


def _hashes_per_block(hash_block_size, digest_size):
    """Digests per hash block, rounded DOWN to a power of two.

    dm-verity indexes with shifts (hash_per_block_bits), so a hash block
    holds 2^floor(log2(hash_block_size/digest_size)) digests -- not
    hash_block_size//digest_size. Identical for sha256 at 4K (128 either
    way), different for e.g. sha1.
    """
    return 1 << ((hash_block_size // digest_size).bit_length() - 1)


def build_hash_tree(image, salt=b"", data_block_size=4096,
                    hash_block_size=4096, algorithm="sha256"):
    """Build the verity tree for `image` (bytes).

    Returns a HashTree whose .tree is the on-disk hash area *without* the
    veritysetup superblock, ready to be written at hash_start_block.
    """
    if len(image) % data_block_size:
        raise ValueError(
            "image is %d bytes, not a multiple of the %d byte data block size"
            % (len(image), data_block_size)
        )
    data_blocks = len(image) // data_block_size
    if data_blocks == 0:
        raise ValueError("image is empty")

    digest_size = hashlib.new(algorithm).digest_size
    per_block = _hashes_per_block(hash_block_size, digest_size)

    def digest(block):
        return hashlib.new(algorithm, salt + block).digest()

    def pack(digests):
        """Concatenate digests and zero pad up to whole hash blocks."""
        blob = b"".join(digests)
        blob += b"\x00" * (-len(blob) % hash_block_size)
        return [blob[i:i + hash_block_size]
                for i in range(0, len(blob), hash_block_size)]

    # Level 0: one digest per data block.
    level = pack(digest(image[i * data_block_size:(i + 1) * data_block_size])
                 for i in range(data_blocks))
    levels = [level]
    while len(levels[-1]) > 1:
        levels.append(pack(digest(block) for block in levels[-1]))

    geometry = (salt, data_block_size, hash_block_size, algorithm)

    if data_blocks == 1:
        # No tree: dm-verity reports 0 hash blocks and the root hash is the
        # lone data block's digest.
        root = digest(image[:data_block_size])
        return HashTree(root.hex(), b"", 0, data_blocks, 0, *geometry)

    root = digest(levels[-1][0])
    # Top level first, level 0 last.
    tree = b"".join(b"".join(blocks) for blocks in reversed(levels))
    return HashTree(root.hex(), tree, len(levels), data_blocks,
                    len(tree) // hash_block_size, *geometry)


def format_hash_area(tree, uuid=None):
    """Superblock block + tree, i.e. what veritysetup writes to a hash device.

    The superblock occupies the first 512 bytes and the tree starts at the
    next hash block boundary, so the area is always a whole number of hash
    blocks.
    """
    if len(tree.salt) > 256:
        raise ValueError("salt is %d bytes, max 256" % len(tree.salt))
    sb = struct.pack(
        _SB_STRUCT,
        SUPERBLOCK_MAGIC,
        1,                                  # superblock version
        HASH_TYPE,
        (_uuid.UUID(uuid) if uuid else _uuid.uuid4()).bytes,
        tree.algorithm.encode(),
        tree.data_block_size,
        tree.hash_block_size,
        tree.data_blocks,
        len(tree.salt),
        b"",                                # _pad1
        tree.salt,
        b"",                                # _pad2
    )
    return sb.ljust(tree.hash_block_size, b"\x00") + tree.tree


def parse_superblock(area):
    """Read back a hash area's superblock. Raises ValueError if it isn't one."""
    if area[:8] != SUPERBLOCK_MAGIC:
        raise ValueError("no verity superblock (magic is %r)" % area[:8])
    (_, version, hash_type, raw_uuid, algorithm, data_block_size,
     hash_block_size, data_blocks, salt_size, _, salt, _) = struct.unpack(
        _SB_STRUCT, area[:SUPERBLOCK_SIZE])
    return {
        "version": version,
        "hash_type": hash_type,
        "uuid": str(_uuid.UUID(bytes=raw_uuid)),
        "algorithm": algorithm.rstrip(b"\x00").decode(),
        "data_block_size": data_block_size,
        "hash_block_size": hash_block_size,
        "data_blocks": data_blocks,
        "salt": salt[:salt_size],
    }


def dm_table(tree, device, hash_offset, hash_device=None):
    """The dm-verity table line for `dmsetup create`.

    `hash_offset` is where the hash *area* begins on the device in bytes --
    for a NeoDCT system image that is the padded squashfs size, since image
    and tree share one device. hash_start_block skips the superblock block.
    """
    if hash_offset % tree.hash_block_size:
        raise ValueError("hash offset %d is not hash-block aligned" % hash_offset)
    hash_start_block = hash_offset // tree.hash_block_size + 1
    sectors = tree.data_blocks * tree.data_block_size // 512
    return " ".join(str(field) for field in (
        0, sectors, "verity", 1, device, hash_device or device,
        tree.data_block_size, tree.hash_block_size, tree.data_blocks,
        hash_start_block, tree.algorithm, tree.root_hash,
        tree.salt.hex() if tree.salt else "-",
    ))
