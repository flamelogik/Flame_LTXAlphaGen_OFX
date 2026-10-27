#!/usr/bin/env bash
# Build the release tarball on a Rocky 9 machine (the .ofx must link against glibc <= 2.34 and
# Flame's GCC 11 libstdc++) and copy it back to dist/.
#   HOST=<ssh host> scripts/package_release.sh  ->  dist/LTXAlphaGen-<version>-rocky9.tar.gz
# Contents: LTXAlphaGen.ofx.bundle (release build), install.sh, README.md, LICENSE.
# Builds in ~/ClaudeCode/LTX_OFX_build/release (separate from the dev/test build); never installs anything.
set -euo pipefail
HOST=${HOST:?set HOST to the ssh name of the Rocky 9 build machine}
REMOTE=${REMOTE:-ClaudeCode/LTX_OFX_build}
SRC="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=$(sed -n 's/^set(LTXAG_VERSION "\([0-9.]*\)")/\1/p' "$SRC/CMakeLists.txt")
[ -n "$VERSION" ] || { echo "no LTXAG_VERSION in CMakeLists.txt" >&2; exit 1; }
NAME="LTXAlphaGen-$VERSION"

# Release gate (systest/README.md): this commit must have passed the system test on Linux
# (scripts/system_test.sh), from a clean checkout. SKIP_SYSTEST=1 overrides (say why in the notes).
if [ "${SKIP_SYSTEST:-0}" != 1 ]; then
  [ -z "$(git -C "$SRC" status --porcelain)" ] ||
    { echo "uncommitted changes: commit, run scripts/system_test.sh, then package (or SKIP_SYSTEST=1)" >&2; exit 1; }
  PASSFILE="$SRC/dist/systest/$(git -C "$SRC" rev-parse --short HEAD)/linux/PASS"
  [ -f "$PASSFILE" ] || { echo "no system-test pass for this commit on Linux ($PASSFILE): run scripts/system_test.sh (or SKIP_SYSTEST=1)" >&2; exit 1; }
fi

ssh "$HOST" "mkdir -p $REMOTE/src"
rsync -a --delete --exclude .git --exclude build --exclude dist --exclude __pycache__ \
  "$SRC/" "$HOST:$REMOTE/src/"
# The git hash for the log's environment snapshot comes from this checkout, not the copy.
GIT=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo nogit)
ssh "$HOST" "set -e; cd $REMOTE
  cmake -S src -B release -DCMAKE_BUILD_TYPE=Release -DLTXAG_DEV_BUILD=OFF -DLTXAG_GIT=$GIT >/dev/null
  cmake --build release -j 8 2>&1 | grep -E 'error|warning' || true
  (cd release && ctest --output-on-failure >/dev/null) || { echo 'ctest failed' >&2; exit 1; }
  rm -rf dist/$NAME && mkdir -p dist/$NAME
  cp -R release/LTXAlphaGen.ofx.bundle dist/$NAME/
  sed 's/^VERSION=dev /VERSION=$VERSION /' src/packaging/install.sh > dist/$NAME/install.sh
  chmod 0755 dist/$NAME/install.sh
  cp src/README.md src/LICENSE dist/$NAME/
  find dist/$NAME -name __pycache__ -prune -exec rm -rf {} +
  tar -C dist -czf dist/$NAME-rocky9.tar.gz $NAME
  echo glibc needed: \$(objdump -T dist/$NAME/LTXAlphaGen.ofx.bundle/Contents/Linux-x86-64/LTXAlphaGen.ofx | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1)"
mkdir -p "$SRC/dist"
scp -q "$HOST:$REMOTE/dist/$NAME-rocky9.tar.gz" "$SRC/dist/"
ls -la "$SRC/dist/$NAME-rocky9.tar.gz"
