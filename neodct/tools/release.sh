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

# ============ THE OTHER TWO VERSION FIELDS HAVE TO AGREE ============
#
# os-release carries the version three times: VERSION_ID (what this script,
# the manifest check and the workflow all read), VERSION, and PRETTY_NAME --
# and PRETTY_NAME is the one the phone prints on its boot banner.
#
# Nothing kept them in step, so they drifted: PRETTY_NAME sat at v0.4.3a from
# 0.4.4a all the way to 0.4.10a, which meant seven releases of a phone that
# booted announcing a version it was not. Everything automated agreed and only
# the human-visible string was wrong, which is exactly the kind of mismatch
# that survives because nothing looks at it.
#
# Refusing to tag is the right response for the same reason the changelog
# check below refuses: a release whose own image disagrees about what it is
# should not be published, and the fix is one line in a file that is already
# open.
V_PLAIN="${VERSION#v}"
for field in VERSION PRETTY_NAME; do
    got=$(sed -n "s/^$field=//p" neodct/overlay/etc/os-release | tr -d '"' | head -n1)
    case "$got" in
        *"$V_PLAIN") ;;
        *) die "os-release $field is \"$got\", which does not end in $V_PLAIN"\
               "-- VERSION_ID says $VERSION; make them agree before tagging" ;;
    esac
done

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
say "pushed $TAG -- the workflow is creating the release"

# --- attach the packages --------------------------------------------------
# Built here, not in CI. A runner would have to build the whole buildroot
# tree, and it would need the signing key -- and an unsigned package
# published as a release is worse than none, because the phone tells
# whoever installs it "BAD SIGNATURE! UPDATE MAY BE CORRUPT!!".
#
# The asset name carries the platform: the phone downloads by that name
# (UpdateService/remote.py asset_name), and one release holds a package
# for each platform that was built.
attach() {   # attach <images-dir>
    # Prefer the archived copy for this exact version: UPDATE.ndsw is a
    # fixed path that the next build overwrites, so it may already be a
    # different version by the time a release is cut.
    pkg="$1/packages/UPDATE-$2-$VERSION.ndsw"
    [ -f "$pkg" ] || pkg="$1/UPDATE.ndsw"
    [ -f "$pkg" ] || return 0
    plat=$(unzip -p "$pkg" manifest.json 2>/dev/null \
           | sed -n 's/.*"platform"[^"]*"\([^"]*\)".*/\1/p' | head -n1)
    ver=$(unzip -p "$pkg" manifest.json 2>/dev/null \
          | sed -n 's/.*"version"[^"]*"\([^"]*\)".*/\1/p' | head -n1)
    [ -n "$plat" ] || { say "cannot read the platform out of $pkg; skipping"; return 0; }
    if [ "$ver" != "$VERSION" ]; then
        say "SKIPPING $pkg: it is $ver, this release is $VERSION"
        say "  rebuild it before releasing, or the phone downloads the wrong image"
        return 0
    fi
    # Signed packages only. An unsigned one built without NEODCT_SIGN_KEY
    # looks identical from the outside.
    if ! unzip -l "$pkg" 2>/dev/null | grep -q "manifest.sig"; then
        say "SKIPPING $pkg: unsigned (built without NEODCT_SIGN_KEY)"
        return 0
    fi
    asset="UPDATE-$plat.ndsw"
    cp "$pkg" "/tmp/$asset"
    say "uploading $asset ($plat)"
    gh release upload "$TAG" "/tmp/$asset" --clobber
    rm -f "/tmp/$asset"
}

if command -v gh > /dev/null 2>&1; then
    # Wait for the workflow to create the release before uploading into it.
    tries=0
    while [ "$tries" -lt 30 ] && ! gh release view "$TAG" > /dev/null 2>&1; do
        sleep 4; tries=$((tries + 1))
    done
    if gh release view "$TAG" > /dev/null 2>&1; then
        attach buildroot/output/images qemu-aarch64
        attach build-luckfox/images luckfox-armv7
        say "assets: $(gh release view "$TAG" --json assets \
                       -q '[.assets[].name] | join(", ")' 2>/dev/null)"
    else
        say "the release did not appear; upload by hand:"
        say "  gh release upload $TAG <file> --clobber"
    fi
else
    say "no gh CLI -- attach the packages by hand"
fi
say "done: gh release view $TAG --web"
