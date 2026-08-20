"""dm-verity hash tree computation.

The values in GOLDEN were produced by cryptsetup's veritysetup 2.8.6, the
reference implementation the kernel's dm-verity target agrees with:

    veritysetup format g.bin gh.bin --data-block-size=4096 \
        --hash-block-size=4096 --salt=<salt>

over `pattern(blocks * 4096)`. They are hardcoded so this suite still has
teeth on a machine with no veritysetup installed; test_matches_veritysetup
re-derives them live when the tool is available.
"""

import hashlib
import os
import shutil
import subprocess

import pytest

from System.core.UpdateService import verity

BS = 4096

# (data_blocks, salt_hex) -> (root_hash, hash_blocks, hash_area_bytes)
GOLDEN = {
    (1, "f8e7d6c5b4a3"): ("c5cc54356d1e31644008f7dd370fae60537bd0f1210bf2669401cef03fbe4cb0", 0, 4096),
    (1, ""): ("5680f46c66c60694d6d13b74989951e91701567b4562c0951db2b9b5ec90ee02", 0, 4096),
    (2, "f8e7d6c5b4a3"): ("67787581d204cb872f5dfd49ff1b9d19b01ef03de1cc38b7d7b6a747fd0ad68f", 1, 8192),
    (2, ""): ("025577dd3cd28bf3727f2ea81096f9e1e0a3622f6415a5205a22be6ee343df4a", 1, 8192),
    (128, "f8e7d6c5b4a3"): ("2acc16db780f2438051083c63619b72ca2cc26a45aa8ba3d21ed28d7e3915087", 1, 8192),
    (128, ""): ("f440b22a57a2294572b768f16bd4518a307f3e76af808940230e55da859255d9", 1, 8192),
    (129, "f8e7d6c5b4a3"): ("53ec2f8e44c1225c7de66ea85cc746eca28723f8b49cda4ed0e1e18fa03b9d99", 3, 16384),
    (129, ""): ("41c4f9acad4084591d573abb22a562f1f09a33d8df01b979e41329b42adb6c13", 3, 16384),
    (300, "f8e7d6c5b4a3"): ("9cd73ab972abc7c21475d94a0e82d6cf8fe68a3ed5ee8536e2695f3a15830ed3", 4, 20480),
    (300, ""): ("5dcdf1b3b4f6806cd2f6c52ad6af67b3ca797ec7c292b3fdfc205fc8ac85dd42", 4, 20480),
}


def pattern(nbytes, seed=b"neodct"):
    """Deterministic, non-trivial test data (same generator as the goldens)."""
    out = bytearray()
    h = seed
    while len(out) < nbytes:
        h = hashlib.sha256(h).digest()
        out += h
    return bytes(out[:nbytes])


def test_root_hash_spans_two_levels():
    """129 data blocks needs a level-0 pair plus a top block above them."""
    image = pattern(129 * BS)
    expected = GOLDEN[(129, "f8e7d6c5b4a3")][0]

    tree = verity.build_hash_tree(image, salt=bytes.fromhex("f8e7d6c5b4a3"))

    assert tree.root_hash == expected


@pytest.mark.parametrize("blocks,salt_hex", sorted(GOLDEN))
def test_root_hash_matches_reference_vectors(blocks, salt_hex):
    expected_root, expected_hash_blocks, _ = GOLDEN[(blocks, salt_hex)]

    tree = verity.build_hash_tree(pattern(blocks * BS),
                                  salt=bytes.fromhex(salt_hex))

    assert tree.root_hash == expected_root
    assert tree.hash_blocks == expected_hash_blocks


def test_single_data_block_has_no_tree():
    """dm-verity reports 0 hash blocks; the root is the leaf digest itself."""
    tree = verity.build_hash_tree(pattern(BS), salt=bytes.fromhex("f8e7d6c5b4a3"))

    assert tree.tree == b""
    assert tree.levels == 0
    assert tree.root_hash == GOLDEN[(1, "f8e7d6c5b4a3")][0]


def test_rejects_image_that_is_not_whole_blocks():
    with pytest.raises(ValueError, match="multiple"):
        verity.build_hash_tree(pattern(BS + 17))


def test_hash_area_starts_with_a_superblock_block():
    """The area is a 4K superblock block, then the tree -- veritysetup's layout."""
    image = pattern(129 * BS)
    tree = verity.build_hash_tree(image, salt=bytes.fromhex("f8e7d6c5b4a3"))

    area = verity.format_hash_area(
        tree, uuid="71e4c695-f7af-47d3-aae3-ea6a0e4ef26c")

    assert len(area) == GOLDEN[(129, "f8e7d6c5b4a3")][2]
    assert area[:8] == b"verity\x00\x00"
    assert area[BS:] == tree.tree


def test_superblock_records_the_geometry():
    tree = verity.build_hash_tree(pattern(300 * BS), salt=b"\x01\x02\x03")

    fields = verity.parse_superblock(verity.format_hash_area(tree))

    assert fields["algorithm"] == "sha256"
    assert fields["data_block_size"] == 4096
    assert fields["hash_block_size"] == 4096
    assert fields["data_blocks"] == 300
    assert fields["salt"] == b"\x01\x02\x03"


@pytest.mark.skipif(shutil.which("veritysetup") is None,
                    reason="veritysetup not installed")
def test_matches_veritysetup_byte_for_byte(tmp_path):
    """Cross-check the whole hash area against the reference implementation."""
    data = tmp_path / "data.img"
    hashes = tmp_path / "hash.img"
    data.write_bytes(pattern(300 * BS))
    out = subprocess.run(
        ["veritysetup", "format", str(data), str(hashes),
         "--data-block-size=4096", "--hash-block-size=4096",
         "--salt=f8e7d6c5b4a3"],
        capture_output=True, text=True, check=True).stdout
    reference = {line.split(":")[0].strip(): line.split()[-1]
                 for line in out.splitlines() if ":" in line}

    tree = verity.build_hash_tree(data.read_bytes(),
                                  salt=bytes.fromhex("f8e7d6c5b4a3"))
    area = verity.format_hash_area(tree, uuid=reference["UUID"])

    assert tree.root_hash == reference["Root hash"]
    assert area == hashes.read_bytes()


def test_dm_table_places_the_tree_after_the_data_and_superblock():
    """One device holds squashfs then hash area, so hash_start skips both."""
    tree = verity.build_hash_tree(pattern(129 * BS),
                                  salt=bytes.fromhex("f8e7d6c5b4a3"))

    table = verity.dm_table(tree, "/dev/vdb", hash_offset=129 * BS)

    assert table == (
        "0 %d verity 1 /dev/vdb /dev/vdb 4096 4096 129 130 sha256 %s f8e7d6c5b4a3"
        % (129 * BS // 512, tree.root_hash)
    )


def test_dm_table_uses_a_dash_for_an_empty_salt():
    tree = verity.build_hash_tree(pattern(2 * BS))

    table = verity.dm_table(tree, "/dev/vdb", hash_offset=2 * BS)

    assert table.endswith(" -")
