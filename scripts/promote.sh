#!/usr/bin/env bash
# Promote a pre-release once it has passed on real hardware (systest/README.md "Releases").
#   scripts/promote.sh <tag>           (what .github/workflows/promote.yml runs; needs gh, python3)
# Requires: the release <tag> exists and is a pre-release; for both linux and macos a result file
# ltxag-test-<platform>-<tag>.json among its assets (scripts/maintainer_test.sh, then
# `gh release upload <tag> ltxag-test-<platform>-<tag>.json`) whose overall is PASS, whose tier is
# full and whose tarball name and sha256 equal the release's own tarball. Then clears the
# pre-release flag, marks the release Latest and puts the hardware results in place of the
# notes' pre-release banner. DRY_RUN=1 checks without changing the release.
# GH_REPO=<owner/repo> selects the repository (default: the one of the current directory).
set -euo pipefail
shopt -s nullglob
TAG=${1:?usage: scripts/promote.sh <tag>}
die() { echo "promote: $*" >&2; exit 1; }
STATE=$(gh release view "$TAG" --json isDraft,isPrerelease --jq '"\(.isDraft) \(.isPrerelease)"') ||
  die "no release $TAG"
[ "${STATE% *}" = false ] || die "$TAG is a draft: publish it as a pre-release first"
[ "${STATE#* }" = true ] || die "$TAG is not a pre-release (already promoted?): nothing to do"

D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
gh release download "$TAG" --dir "$D" --pattern 'LTXAlphaGen-*.tar.gz' --pattern 'ltxag-test-*.json' ||
  die "can't download the assets of $TAG"
if command -v sha256sum >/dev/null; then sha256() { sha256sum "$1" | cut -d' ' -f1; }
else sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }; fi

BAD=0
for P in linux:rocky9 macos:macos-arm64; do
  PLATFORM=${P%%:*} SUFFIX=${P#*:}
  set -- "$D"/LTXAlphaGen-*-"$SUFFIX".tar.gz
  [ $# = 1 ] && [ -f "$1" ] || { echo "FAIL $PLATFORM: expected one *-$SUFFIX.tar.gz asset, found $# ($*)"; BAD=1; continue; }
  TARBALL=$(basename "$1") SHA=$(sha256 "$1")
  RESULT="$D/ltxag-test-$PLATFORM-$TAG.json"
  if [ ! -f "$RESULT" ]; then
    echo "FAIL $PLATFORM: no ltxag-test-$PLATFORM-$TAG.json on the release. On a $PLATFORM test machine run"
    echo "     scripts/maintainer_test.sh $TAG && gh release upload $TAG ltxag-test-$PLATFORM-$TAG.json"
    BAD=1; continue
  fi
  SUMMARY=$D/summary RESULT=$RESULT TAG=$TAG PLATFORM=$PLATFORM TARBALL=$TARBALL SHA=$SHA python3 -I - <<'EOF' || BAD=1
import json, os, sys
e = os.environ
try:
    r = json.load(open(e["RESULT"]))
except ValueError as x:
    sys.exit("FAIL %s: unreadable result file: %s" % (e["PLATFORM"], x))
want = {"tag": e["TAG"], "platform": e["PLATFORM"], "tier": "full", "tarball": e["TARBALL"],
        "tarball_sha256": e["SHA"], "overall": "PASS"}
bad = ["%s is %r, needs %r" % (k, r.get(k), v) for k, v in want.items() if r.get(k) != v]
if bad:
    sys.exit("FAIL %s: %s" % (e["PLATFORM"], "; ".join(bad)))
m = r.get("machine", {})
res = r.get("results", {}).get("full", {})
line = "%s: %s passed, %s failed, %s skipped on %s, %s (%s, sha256 %s...)" % (
    e["PLATFORM"], res.get("passed"), res.get("failures"), res.get("skipped"), m.get("os"), m.get("gpu"),
    r.get("timestamp"), e["SHA"][:12])
print("ok   " + line)
with open(e["SUMMARY"], "a") as f:
    f.write("> - " + line + "\n")
EOF
done
[ $BAD = 0 ] || die "$TAG stays a pre-release"
if [ "${DRY_RUN:-0}" = 1 ]; then echo "DRY_RUN: $TAG would be promoted"; exit 0; fi
# New notes: the hardware results in place of the pre-release banner (the leading "> " block).
{
  echo "> **Verified on real hardware** (system test, full tier with the real model; result files attached):"
  cat "$D/summary"
  echo
  gh release view "$TAG" --json body --jq .body | awk 'banner && /^>/ {next} {banner=0; print}' banner=1
} >"$D/notes.md"
gh release edit "$TAG" --prerelease=false --latest --notes-file "$D/notes.md"
echo "promoted $TAG: no longer a pre-release, marked Latest"
