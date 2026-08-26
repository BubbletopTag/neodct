# Host-side unit tests for NeoDCT (never shipped to the target).
# Make /NeoDCT-style absolute imports (System.hw..., System.ui...) work
# by putting the overlay root on sys.path, same as the device runtime.
import os
import sys

# The Python OS is the reference implementation and lives outside the
# overlay, so BR2_ROOTFS_OVERLAY cannot put it on a phone. Importing
# System.* is importing that reference.
OVERLAY_NEODCT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "python-reference",
)
if OVERLAY_NEODCT not in sys.path:
    sys.path.insert(0, OVERLAY_NEODCT)
