#!/bin/sh
# Cut a release: check, tag, push. The workflow does the rest.
#
#   neodct/tools/release.sh            tag the version in os-release
#   neodct/tools/release.sh --dry-run  say what it would do
#
# The version is never passed in. It comes from VERSION_ID in
# neodct/overlay/etc/os-release, which is the same field the image reports
# and the manifest is compared against -- a release tagged by hand can
# disagree with the image it contains, and that is exactly the failure the
# workflow refuses to publish.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$(dirname "$HERE")")"
cd "$REPO"

DRY=""
[ "${1:-}" = "--dry-run" ] && DRY=1

say() { echo "[release] $*"; }
die() { echo "[release] $*" >&2; exit 1; }

VERSION=$(sed -n 's/^VERSION_ID=//p' neodct/overlay/etc/os-release \
          | tr -d '"' | head -n1)
[ -n "$VERSION" ] || die "no VERSION_ID in neodct/overlay/etc/os-release"

# Recent tags carry no leading v (0.2.3a); the oldest ones do. Follow the
# recent convention and let the workflow accept both.
TAG="$VERSION"

say "version: $VERSION"

# A changelog section is not optional: it is the release's entire body.
if ! grep -qx "$VERSION" neodct/overlay/NeoDCT/CHANGELOG.txt; then
    die "CHANGELOG.txt has no section headed exactly '$VERSION'"
fi

if [ -n "$(git status --porcelain)" ]; then
    die "working tree is dirty -- commit before tagging"
fi

if git rev-parse "$TAG" > /dev/null 2>&1; then
    die "tag $TAG already exists (bump VERSION_ID first)"
fi

BRANCH=$(git rev-parse --abbrev-ref HEAD)
say "branch:  $BRANCH"
say "tag:     $TAG"

if [ -n "$DRY" ]; then
    say "dry run -- would tag $TAG and push it to origin"
    say "notes would be the '$VERSION' section of CHANGELOG.txt:"
    awk -v v="$VERSION" '
        $0 == v { grab = 1; next }
        grab && /^[0-9]+\.[0-9]+\.[0-9]+[a-z]?$/ { exit }
        grab { print "    " $0 }
    ' neodct/overlay/NeoDCT/CHANGELOG.txt
    exit 0
fi

git tag -a "$TAG" -m "NeoDCT OS $VERSION"
say "tagged $TAG"
git push origin "$TAG"
say "pushed $TAG -- the Release workflow takes it from here"
say "watch it: gh run watch"
