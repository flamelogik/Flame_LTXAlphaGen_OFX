#!/usr/bin/env bash
# Test a release package (or a dev build) on THIS machine and write a result file for promotion
# (systest/README.md "Releases").
#   scripts/maintainer_test.sh <tag | path/to/LTXAlphaGen-<v>-<platform>.tar.gz>
#       [--sha256 <expected hash>] [--tag <release tag>] [--repo <owner/repo>] [--work <dir>] [--out <dir>]
#     Full tier (real model) on the packaged release bundle. A tag is downloaded with
#     `gh release download`; a tarball is used as given (its tag is taken from the name unless --tag).
#     -> <out>/ltxag-test-<linux|macos>-<tag>.json; upload it with
#        gh release upload <tag> ltxag-test-<linux|macos>-<tag>.json
#   scripts/maintainer_test.sh --dev [--work <dir>] [--out <dir>]
#     Fast tier (dummy worker; no GPU, model or Setup needed) on a dev build of this checkout.
#     For contributors; the result (ltxag-test-<platform>-dev-<commit>.json) is not used for promotion.
# The full tier needs a completed Setup for the user running it (Setup in Flame: runtime, models,
# prompt cache). The test host is built from this checkout: check out the release tag first.
# <dir> (default ~/.cache/ltx-alphagen/maintainer-test) must be on a local disk.
# Environment: CMAKE, CMAKE_ARGS (extra configure options, shell-quoted, as for
# package_release.sh), JOB_PYTHON (python with numpy that runs job.py; default /usr/bin/python3 on
# Linux, Setup's ltx_python on macOS).
set -uo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
CMAKE=${CMAKE:-cmake}
CMAKE_ARGS=${CMAKE_ARGS:-}
die() { echo "maintainer_test: $*" >&2; exit 2; }
usage() { sed -n '2,20p' "$0" >&2; exit 2; }

DEV=0 INPUT= SHA_EXPECTED= TAG= REPO= WORK= OUT=$PWD
while [ $# -gt 0 ]; do
  case $1 in
    --dev) DEV=1; shift ;;
    --sha256) SHA_EXPECTED=$2; shift 2 ;; --tag) TAG=$2; shift 2 ;; --repo) REPO=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;; --out) OUT=$2; shift 2 ;;
    -h|--help) usage ;;
    -*) die "unknown option $1" ;;
    *) [ -z "$INPUT" ] || die "one tag or tarball only"; INPUT=$1; shift ;;
  esac
done
{ [ $DEV = 1 ] && [ -z "$INPUT" ]; } || { [ $DEV = 0 ] && [ -n "$INPUT" ]; } || usage

case $(uname -s) in
  Linux) PLATFORM=linux SUFFIX=rocky9 ARCH=Linux-x86-64 ;;
  Darwin) PLATFORM=macos SUFFIX=macos-arm64 ARCH=MacOS ;;
  *) die "no release for $(uname -s)" ;;
esac
command -v python3 >/dev/null || die "python3 is needed for the result file"
if command -v sha256sum >/dev/null; then sha256() { sha256sum "$1" | cut -d' ' -f1; }
else sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }; fi
WORK=${WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/ltx-alphagen/maintainer-test}
mkdir -p "$WORK" "$OUT" || die "can't create $WORK or $OUT"
WORK="$(cd "$WORK" && pwd)" OUT="$(cd "$OUT" && pwd)"
eval "EXTRA=($CMAKE_ARGS)"
HOST_COMMIT=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)
[ -n "$(git -C "$SRC" status --porcelain 2>/dev/null)" ] && HOST_COMMIT="$HOST_COMMIT-dirty"

# build <folder> <ON|OFF>: the test host (ltxag_systest; CMake builds a bundle next to it as well).
build() {
  echo "== building the test host in $1 (dev build $2)"
  { "$CMAKE" -S "$SRC" -B "$1" -DCMAKE_BUILD_TYPE=Release -DLTXAG_DEV_BUILD="$2" -DLTXAG_GIT="$HOST_COMMIT" \
      ${EXTRA[@]+"${EXTRA[@]}"} && "$CMAKE" --build "$1" -j 8 --target ltxag_systest; } >"$1.log" 2>&1 ||
    { tail -30 "$1.log"; die "build failed ($1.log)"; }
}

TARBALL_NAME= TARBALL_SHA= VERSION=
if [ $DEV = 1 ]; then
  TIER=fast
  build "$WORK/dev-build" ON
  BUNDLE="$WORK/dev-build/LTXAlphaGen.ofx.bundle"
  TAG="dev-$HOST_COMMIT"
  SOURCE_COMMIT=$HOST_COMMIT
  VERSION=$(sed -n 's/^set(LTXAG_VERSION "\([0-9.]*\)")/\1/p' "$SRC/CMakeLists.txt")
else
  TIER=full
  if [ -f "$INPUT" ]; then
    TARBALL="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"
    if [ -z "$TAG" ]; then
      TAG=$(basename "$TARBALL" | sed -n "s/^LTXAlphaGen-\([0-9.]*\)-$SUFFIX\.tar\.gz$/v\1/p")
      [ -n "$TAG" ] || die "can't tell the tag from $(basename "$TARBALL"): pass --tag"
    fi
  else
    TAG=$INPUT
    command -v gh >/dev/null || die "gh (GitHub CLI) is needed to download $TAG; or pass a tarball"
    rm -rf "$WORK/download" && mkdir -p "$WORK/download"
    echo "== downloading the $SUFFIX package of $TAG"
    (cd "$SRC" && gh release download "$TAG" ${REPO:+--repo "$REPO"} --dir "$WORK/download" \
      --pattern "LTXAlphaGen-*-$SUFFIX.tar.gz" --pattern "LTXAlphaGen-*-$SUFFIX.tar.gz.sha256") ||
      die "gh release download $TAG failed"
    TARBALL=$(ls "$WORK/download/"*"-$SUFFIX.tar.gz" 2>/dev/null | head -1)
    [ -n "$TARBALL" ] || die "release $TAG has no $SUFFIX tarball"
  fi
  TARBALL_NAME=$(basename "$TARBALL")
  TARBALL_SHA=$(sha256 "$TARBALL")
  echo "== $TARBALL_NAME sha256 $TARBALL_SHA"
  if [ -f "$TARBALL.sha256" ]; then
    [ "$(cut -d' ' -f1 "$TARBALL.sha256")" = "$TARBALL_SHA" ] || die "sha256 differs from $TARBALL_NAME.sha256"
  fi
  [ -z "$SHA_EXPECTED" ] || [ "$SHA_EXPECTED" = "$TARBALL_SHA" ] || die "sha256 differs from --sha256 $SHA_EXPECTED"
  rm -rf "$WORK/pkg" && mkdir -p "$WORK/pkg"
  tar -xzf "$TARBALL" -C "$WORK/pkg" || die "can't extract $TARBALL_NAME"
  BUNDLE=$(ls -d "$WORK/pkg/"*/LTXAlphaGen.ofx.bundle 2>/dev/null | head -1)
  [ -n "$BUNDLE" ] || die "no LTXAlphaGen.ofx.bundle in $TARBALL_NAME"
  # The Version label holds "<version> (<commit>)" (ofx/src/plugin.cpp, LTXAG_GIT).
  LABEL=$(LC_ALL=C grep -aoE '[0-9]+\.[0-9]+\.[0-9]+ \([0-9A-Za-z-]+\)' "$BUNDLE/Contents/$ARCH/LTXAlphaGen.ofx" | head -1)
  VERSION=${LABEL%% *}
  SOURCE_COMMIT=$(echo "$LABEL" | sed -n 's/.*(\(.*\))/\1/p')
  [ "${HOST_COMMIT%-dirty}" = "${SOURCE_COMMIT%-dirty}" ] ||
    echo "warning: the test host is built from $HOST_COMMIT, the release from ${SOURCE_COMMIT:-unknown}; check out $TAG for a matching test" >&2
  build "$WORK/host-build" OFF
fi
HOSTBIN=$(dirname "$BUNDLE")/ltxag_systest
[ $DEV = 0 ] && HOSTBIN="$WORK/host-build/ltxag_systest"

ARGS=(--tier "$TIER" --bundle "$BUNDLE" --host "$HOSTBIN" --work "$WORK/run")
[ -n "${JOB_PYTHON:-}" ] && ARGS+=(--job-python "$JOB_PYTHON")
"$SRC/systest/run_systest.sh" "${ARGS[@]}"
RC=$?

# Machine description (no host or user names: the file is published with the release).
if [ $PLATFORM = linux ]; then
  OS=$(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME"); OS="$OS, kernel $(uname -r)"
  CPU=$(sed -n 's/^model name\s*: //p' /proc/cpuinfo | head -1)
  RAM=$(awk '/^MemTotal/ {printf "%.0f", $2 / 1048576}' /proc/meminfo)
  GPU=$(nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader 2>/dev/null | paste -sd';' -)
else
  OS="macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
  CPU=$(sysctl -n machdep.cpu.brand_string)
  RAM=$(( $(sysctl -n hw.memsize) / 1073741824 ))
  GPU="$CPU (unified memory)"
fi
RESULT="$OUT/ltxag-test-$PLATFORM-$TAG.json"
JUNIT="$WORK/run/out-$TIER/junit-$TIER.xml" \
TAG="$TAG" PLATFORM="$PLATFORM" TIER="$TIER" TARBALL_NAME="$TARBALL_NAME" TARBALL_SHA="$TARBALL_SHA" \
VERSION="$VERSION" SOURCE_COMMIT="$SOURCE_COMMIT" HOST_COMMIT="$HOST_COMMIT" RC="$RC" \
OS="$OS" CPU="$CPU" RAM="$RAM" GPU="$GPU" RESULT="$RESULT" python3 -I - <<'EOF'
import datetime, json, os, xml.etree.ElementTree as ET
e = os.environ
res = {"tests": 0, "passed": 0, "failures": 0, "skipped": 0, "time_s": 0.0, "failed_tests": []}
try:
    s = ET.parse(e["JUNIT"]).getroot()
    res.update(tests=int(s.get("tests")), failures=int(s.get("failures")), skipped=int(s.get("skipped")),
               time_s=round(float(s.get("time")), 1),
               failed_tests=[c.get("name") for c in s.iter("testcase") if c.find("failure") is not None])
    res["passed"] = res["tests"] - res["failures"] - res["skipped"]
except (OSError, ET.ParseError, TypeError, ValueError) as x:
    res["error"] = "no readable %s (%s)" % (os.path.basename(e["JUNIT"]), type(x).__name__)
res["exit_code"] = int(e["RC"])
ok = res["exit_code"] == 0 and res["failures"] == 0 and res["passed"] > 0 and "error" not in res
doc = {
    "schema": 1, "tag": e["TAG"], "platform": e["PLATFORM"], "tier": e["TIER"],
    "tarball": e["TARBALL_NAME"] or None, "tarball_sha256": e["TARBALL_SHA"] or None,
    "version": e["VERSION"], "source_commit": e["SOURCE_COMMIT"] or None, "test_host_commit": e["HOST_COMMIT"],
    "results": {e["TIER"]: res},
    "machine": {"os": e["OS"], "cpu": e["CPU"], "ram_gb": int(e["RAM"] or 0), "gpu": e["GPU"] or None},
    "timestamp": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "overall": "PASS" if ok else "FAIL",
}
with open(e["RESULT"], "w") as f:
    json.dump(doc, f, indent=2)
    f.write("\n")
print("%s: %s tier %s (%d passed, %d failed, %d skipped)" % (e["RESULT"], e["TIER"], doc["overall"],
      res["passed"], res["failures"], res["skipped"]))
EOF
grep -q '"overall": "PASS"' "$RESULT" || {
  echo "logs: $WORK/run/out-$TIER/ (console.log, logs.tar.gz)"; exit 1; }
[ $DEV = 0 ] && echo "upload: gh release upload $TAG $RESULT"
exit 0
