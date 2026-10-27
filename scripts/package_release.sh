#!/usr/bin/env bash
# Build the release bundle, run ctest and pack the release tarball (+ .sha256) into dist/.
#   scripts/package_release.sh                     on this machine (what CI runs)
#   HOST=<ssh host> [REMOTE=<folder there>] scripts/package_release.sh
#                                                   on a build machine over ssh; copies the result back
# Linux -> dist/LTXAlphaGen-<version>-rocky9.tar.gz: must be built on Rocky 9 (glibc <= 2.34,
#          GCC 11, CLAUDE.md "Building"); a binary that needs a newer glibc is refused.
# macOS -> dist/LTXAlphaGen-<version>-macos-arm64.tar.gz (Apple Silicon).
# Contents: LTXAlphaGen.ofx.bundle (release build), install.sh, README.md, LICENSE, NOTICE.
# Environment: CMAKE (default cmake; ctest is taken from next to it), CMAKE_ARGS (extra configure
#   options, shell-quoted: -DWORKER_TEST_PYTHON=<python with numpy>, the macOS SDK workarounds in
#   CLAUDE.md), BUILD (build folder, default <checkout>/build-release; over ssh $REMOTE/release),
#   GIT (build name, default this checkout's short hash, "-dirty" with uncommitted changes).
# The system test is not a gate here: a release is tested on real hardware before it is promoted
# (systest/README.md "Releases"). Never installs anything.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
CMAKE=${CMAKE:-cmake}
if [[ $CMAKE == */* ]]; then CTEST=${CTEST:-${CMAKE%/*}/ctest}; else CTEST=${CTEST:-ctest}; fi
CMAKE_ARGS=${CMAKE_ARGS:-}
if [ -z "${GIT:-}" ]; then
  GIT=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo nogit)
  [ -n "$(git -C "$SRC" status --porcelain 2>/dev/null)" ] && GIT="$GIT-dirty"
fi
VERSION=$(sed -n 's/^set(LTXAG_VERSION "\([0-9.]*\)")/\1/p' "$SRC/CMakeLists.txt")
[ -n "$VERSION" ] || { echo "no LTXAG_VERSION in CMakeLists.txt" >&2; exit 1; }
NAME="LTXAlphaGen-$VERSION"

# Over ssh: sync this checkout, run this script there in local mode, copy the tarball back.
if [ -n "${HOST:-}" ]; then
  REMOTE=${REMOTE:-ClaudeCode/LTX_OFX_build}
  case $(ssh "$HOST" uname -s) in Darwin) TARBALL=$NAME-macos-arm64.tar.gz ;; *) TARBALL=$NAME-rocky9.tar.gz ;; esac
  ssh "$HOST" "mkdir -p '$REMOTE/src'"
  rsync -a --delete --exclude .git --exclude /build --exclude '/build-*' --exclude dist --exclude docs \
    --exclude __pycache__ --exclude CLAUDE.local.md --exclude .agent-status "$SRC/" "$HOST:$REMOTE/src/"
  ssh "$HOST" "cd '$REMOTE/src' && GIT=$(printf %q "$GIT") CMAKE=$(printf %q "$CMAKE") \
    CMAKE_ARGS=$(printf %q "$CMAKE_ARGS") BUILD=$(printf %q "${BUILD:-../release}") \
    bash scripts/package_release.sh"
  mkdir -p "$SRC/dist"
  scp -q "$HOST:$REMOTE/src/dist/$TARBALL" "$HOST:$REMOTE/src/dist/$TARBALL.sha256" "$SRC/dist/"
  ls -la "$SRC/dist/$TARBALL" "$SRC/dist/$TARBALL.sha256"
  exit 0
fi

case $(uname -s) in
  Linux) SUFFIX=rocky9 ARCH=Linux-x86-64 ;;
  Darwin) SUFFIX=macos-arm64 ARCH=MacOS ;;
  *) echo "no release package for $(uname -s)" >&2; exit 1 ;;
esac
BUILD=${BUILD:-$SRC/build-release}
mkdir -p "$BUILD" && BUILD="$(cd "$BUILD" && pwd)"
eval "EXTRA=($CMAKE_ARGS)"
echo "building $NAME ($GIT) in $BUILD"
"$CMAKE" -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DLTXAG_DEV_BUILD=OFF -DLTXAG_GIT="$GIT" \
  ${EXTRA[@]+"${EXTRA[@]}"} >"$BUILD/configure.log" 2>&1 || { tail -30 "$BUILD/configure.log"; echo "configure failed" >&2; exit 1; }
"$CMAKE" --build "$BUILD" -j 8 >"$BUILD/build.log" 2>&1 || { grep -E 'error' "$BUILD/build.log" | head -30; echo "build failed ($BUILD/build.log)" >&2; exit 1; }
grep -E 'warning' "$BUILD/build.log" || true
(cd "$BUILD" && "$CTEST" --output-on-failure) || { echo "ctest failed" >&2; exit 1; }

BIN="$BUILD/LTXAlphaGen.ofx.bundle/Contents/$ARCH/LTXAlphaGen.ofx"
if [ "$SUFFIX" = rocky9 ]; then
  GLIBC=$(objdump -T "$BIN" | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1)
  echo "glibc needed: $GLIBC; libstdc++ needed: $(objdump -T "$BIN" | grep -o 'GLIBCXX_[0-9.]*' | sort -V | tail -1)"
  [ "$(printf '%s\n' "$GLIBC" GLIBC_2.34 | sort -V | tail -1)" = GLIBC_2.34 ] ||
    { echo "$GLIBC is newer than Rocky 9's GLIBC_2.34: Flame can't load this; build on Rocky 9" >&2; exit 1; }
else
  echo "binary: $(file "$BIN" | cut -d: -f2)"
  # The bundle must load on macOS 15 (CMakeLists.txt pins it); a build that says more is refused.
  MINOS=$(otool -l "$BIN" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
  echo "minos: $MINOS"
  [ "$MINOS" = 15.0 ] || { echo "minos $MINOS, expected 15.0: set CMAKE_OSX_DEPLOYMENT_TARGET=15.0" >&2; exit 1; }
fi

DIST="$SRC/dist"
TARBALL="$NAME-$SUFFIX.tar.gz"
rm -rf "${DIST:?}/$NAME" && mkdir -p "$DIST/$NAME"
cp -R "$BUILD/LTXAlphaGen.ofx.bundle" "$DIST/$NAME/"
sed "s/^VERSION=dev /VERSION=$VERSION /" "$SRC/packaging/install.sh" >"$DIST/$NAME/install.sh"
chmod 0755 "$DIST/$NAME/install.sh"
cp "$SRC/README.md" "$SRC/LICENSE" "$SRC/NOTICE" "$DIST/$NAME/"
find "$DIST/$NAME" -name __pycache__ -prune -exec rm -rf {} +
COPYFILE_DISABLE=1 tar -C "$DIST" -czf "$DIST/$TARBALL" "$NAME"
if command -v sha256sum >/dev/null; then SHA="sha256sum"; else SHA="shasum -a 256"; fi
(cd "$DIST" && $SHA "$TARBALL" >"$TARBALL.sha256")
cat "$DIST/$TARBALL.sha256"
echo "tarball: $TARBALL"
