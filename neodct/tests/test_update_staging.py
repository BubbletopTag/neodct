"""The handoff between SystemUpdate (writer) and the initramfs (reader).

State lives in KEY=value files rather than JSON because the applier that
consumes them is a busybox shell script -- see neodct/initramfs/init.
"""

import os

import pytest

from System.core.UpdateService import manifest as manifest_mod
from System.core.UpdateService import staging

from update_fixtures import build_image, make_ndsw, pattern


def staged_manifest(tmp_path, **overrides):
    """A parsed manifest for a real image written to disk."""
    image, tree = build_image()
    body = make_ndsw(tmp_path / "src.ndsw", image=image, tree=tree, **overrides)
    import json
    return manifest_mod.parse(json.dumps(body).encode()), image


def test_staging_records_everything_the_applier_needs(tmp_path):
    state = tmp_path / ".ndsys"
    parsed, image = staged_manifest(tmp_path)
    (tmp_path / "pending.img").write_bytes(image)

    staging.stage(parsed, tmp_path / "pending.img", state)
    pending = staging.read_pending(state)

    assert pending.version == "0.3.2a"
    assert pending.sha256 == parsed.sha256
    assert pending.verity_root_hash == parsed.verity["root_hash"]
    assert pending.verity_block_size == 4096
    assert pending.verity_image_blocks == parsed.verity["image_blocks"]
    assert pending.platform == "qemu-aarch64"
    assert pending.attempts == 0
    assert os.path.exists(pending.image)


def test_the_record_says_how_many_bytes_the_image_is(tmp_path):
    """The applier dd's the image out and hashes exactly that many bytes back
    off the device to prove the write landed -- it cannot guess the length
    from the partition size."""
    state = tmp_path / ".ndsys"
    parsed, image = staged_manifest(tmp_path)
    (tmp_path / "pending.img").write_bytes(image)

    staging.stage(parsed, tmp_path / "pending.img", state)

    assert staging.read_pending(state).image_bytes == len(image)
    # The squashfs is only part of it; the verity tree follows.
    assert len(image) > parsed.hash_offset


def test_the_staged_image_is_moved_not_copied(tmp_path):
    """A 50MB image must not need double the space to stage."""
    state = tmp_path / ".ndsys"
    parsed, image = staged_manifest(tmp_path)
    source = tmp_path / "pending.img"
    source.write_bytes(image)

    staging.stage(parsed, source, state)

    assert not source.exists()


def test_there_is_no_pending_update_on_a_clean_system(tmp_path):
    assert staging.read_pending(tmp_path / ".ndsys") is None


def test_an_image_with_no_record_is_not_a_pending_update(tmp_path):
    """The record file is the commit point, so it is written last."""
    state = tmp_path / ".ndsys"
    state.mkdir()
    (state / "pending.img").write_bytes(pattern(4096))

    assert staging.read_pending(state) is None


def test_a_record_with_no_image_is_not_a_pending_update(tmp_path):
    state = tmp_path / ".ndsys"
    parsed, image = staged_manifest(tmp_path)
    (tmp_path / "pending.img").write_bytes(image)
    staging.stage(parsed, tmp_path / "pending.img", state)
    os.unlink(state / "pending.img")

    assert staging.read_pending(state) is None


def test_a_truncated_record_is_ignored_rather_than_crashing_the_boot(tmp_path):
    state = tmp_path / ".ndsys"
    parsed, image = staged_manifest(tmp_path)
    (tmp_path / "pending.img").write_bytes(image)
    staging.stage(parsed, tmp_path / "pending.img", state)
    (state / "pending.prop").write_text("version=0.3.2a\n")  # no hashes

    assert staging.read_pending(state) is None


def test_clearing_removes_both_the_record_and_the_image(tmp_path):
    state = tmp_path / ".ndsys"
    parsed, image = staged_manifest(tmp_path)
    (tmp_path / "pending.img").write_bytes(image)
    staging.stage(parsed, tmp_path / "pending.img", state)

    staging.clear_pending(state)

    assert staging.read_pending(state) is None
    assert not (state / "pending.img").exists()
    assert not (state / "pending.prop").exists()


def test_clearing_a_clean_system_is_harmless(tmp_path):
    staging.clear_pending(tmp_path / ".ndsys")  # must not raise


def test_staging_twice_replaces_the_earlier_pending_update(tmp_path):
    state = tmp_path / ".ndsys"
    first, image = staged_manifest(tmp_path, version="0.3.1a")
    (tmp_path / "a.img").write_bytes(image)
    staging.stage(first, tmp_path / "a.img", state)
    second, image2 = staged_manifest(tmp_path, version="0.4.0a")
    (tmp_path / "b.img").write_bytes(image2)

    staging.stage(second, tmp_path / "b.img", state)

    assert staging.read_pending(state).version == "0.4.0a"
    assert len([n for n in os.listdir(state) if n.endswith(".img")]) == 1


def test_attempts_climb_so_a_failing_apply_cannot_loop_forever(tmp_path):
    state = tmp_path / ".ndsys"
    parsed, image = staged_manifest(tmp_path)
    (tmp_path / "pending.img").write_bytes(image)
    staging.stage(parsed, tmp_path / "pending.img", state)

    assert staging.note_attempt(state) == 1
    assert staging.note_attempt(state) == 2
    assert staging.read_pending(state).attempts == 2


def test_a_pending_update_is_abandoned_after_three_attempts(tmp_path):
    state = tmp_path / ".ndsys"
    parsed, image = staged_manifest(tmp_path)
    (tmp_path / "pending.img").write_bytes(image)
    staging.stage(parsed, tmp_path / "pending.img", state)
    for _ in range(3):
        staging.note_attempt(state)

    pending = staging.read_pending(state)

    assert pending is not None
    assert pending.exhausted is True


def test_records_the_installed_system_for_the_next_boot(tmp_path):
    state = tmp_path / ".ndsys"
    parsed, _ = staged_manifest(tmp_path)

    staging.record_installed(parsed, state)
    installed = staging.read_installed(state)

    assert installed.version == "0.3.2a"
    assert installed.verity_root_hash == parsed.verity["root_hash"]


def test_no_installed_record_before_first_provisioning(tmp_path):
    assert staging.read_installed(tmp_path / ".ndsys") is None


def test_the_applier_can_report_how_it_went(tmp_path):
    state = tmp_path / ".ndsys"

    staging.record_result(state, "failed", version="0.3.2a",
                          reason="image sha256 mismatch")
    result = staging.read_result(state)

    assert result["result"] == "failed"
    assert result["version"] == "0.3.2a"
    assert result["reason"] == "image sha256 mismatch"


def test_a_result_can_be_acknowledged_once_shown(tmp_path):
    state = tmp_path / ".ndsys"
    staging.record_result(state, "ok", version="0.3.2a")

    staging.clear_result(state)

    assert staging.read_result(state) is None


def test_records_survive_a_read_only_root(tmp_path):
    """Everything must sit under the User partition, never under /NeoDCT/System."""
    assert staging.STATE_DIR.startswith("/NeoDCT/User/")


def test_values_with_awkward_characters_round_trip(tmp_path):
    """A changelog full of shell metacharacters must not corrupt the record."""
    state = tmp_path / ".ndsys"

    staging.record_result(state, "failed", version="0.3.2a",
                          reason="$(reboot) `rm -rf /` 'quoted' \"double\"")

    assert staging.read_result(state)["reason"] == \
        "$(reboot) `rm -rf /` 'quoted' \"double\""


def test_newlines_in_values_are_refused_rather_than_silently_split(tmp_path):
    state = tmp_path / ".ndsys"

    with pytest.raises(ValueError, match="newline"):
        staging.record_result(state, "failed", reason="line one\nline two")


def test_the_image_path_is_resolved_against_the_state_directory(tmp_path):
    """A record written on the phone names /NeoDCT/User/.ndsys/pending.img,
    which does not exist in the initramfs (or in a test). Resolve it where
    the record actually lives instead of trusting the string."""
    state = tmp_path / ".ndsys"
    parsed, image = staged_manifest(tmp_path)
    (tmp_path / "pending.img").write_bytes(image)
    staging.stage(parsed, tmp_path / "pending.img", state)
    record = state / "pending.prop"
    record.write_text("".join(
        "image=/NeoDCT/User/.ndsys/pending.img\n" if line.startswith("image=")
        else line
        for line in record.read_text().splitlines(keepends=True)))

    pending = staging.read_pending(state)

    assert pending is not None
    assert pending.image == str(state / "pending.img")
    assert os.path.exists(pending.image)
