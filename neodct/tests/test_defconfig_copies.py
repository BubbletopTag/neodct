"""The two copies of every defconfig have to stay identical.

AGENTS.md lists this first under Gotchas, and it is the one the project has
no automated guard for: `buildroot/configs/` is what the build reads and
`neodct/configs/` is the copy that lives with the rest of the project, so an
edit to one and not the other means the next person builds something other
than what they are looking at. The advice is "edit both, or the next person
builds something else. Verify with `diff` before committing" -- which is a
diff somebody has to remember to run.

Worth having now in particular: SECURITY-PLAN.md section 4 takes four
packages out one commit at a time, which is eight edits across four files in
four commits. That is exactly the shape that drifts.
"""

import os

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NEODCT_CONFIGS = os.path.join(REPO, "neodct", "configs")
BUILDROOT_CONFIGS = os.path.join(REPO, "buildroot", "configs")

DEFCONFIGS = sorted(name for name in os.listdir(NEODCT_CONFIGS)
                    if name.endswith("_defconfig"))


def test_there_are_defconfigs_to_compare():
    """If this list is ever empty the parametrised tests below all pass by
    vacuum, which would be the worst possible way for this file to fail."""
    assert DEFCONFIGS, NEODCT_CONFIGS


@pytest.mark.parametrize("name", DEFCONFIGS)
def test_the_build_copy_exists(name):
    assert os.path.exists(os.path.join(BUILDROOT_CONFIGS, name)), (
        "%s is in neodct/configs but not in buildroot/configs, which is the "
        "one the build actually reads" % name)


@pytest.mark.parametrize("name", DEFCONFIGS)
def test_the_two_copies_are_identical(name):
    ours = open(os.path.join(NEODCT_CONFIGS, name)).read()
    theirs = open(os.path.join(BUILDROOT_CONFIGS, name)).read()

    assert ours == theirs, (
        "neodct/configs/%s and buildroot/configs/%s have diverged. The build "
        "reads the buildroot copy; edit both." % (name, name))
