#!/usr/bin/env bash
# Sync the source to a Rocky 9 build machine, build and test there.
#   HOST=<ssh host> scripts/build_remote.sh
# Builds in the user's home only; never touches /usr/OFX/Plugins or the live
# ComfyUI install. The one-time install symlink is done by hand with sudo:
#   sudo ln -s ~/ClaudeCode/LTX_OFX_build/build/LTXAlphaGen.ofx.bundle /usr/OFX/Plugins/
set -euo pipefail
HOST=${HOST:?set HOST to the ssh name of the Rocky 9 build machine}
REMOTE=${REMOTE:-ClaudeCode/LTX_OFX_build}
SRC="$(cd "$(dirname "$0")/.." && pwd)"

ssh "$HOST" "mkdir -p $REMOTE/src"
rsync -a --delete --exclude .git --exclude build --exclude __pycache__ \
  "$SRC/" "$HOST:$REMOTE/src/"
# Release UI by default (user, 2026-10-11: no Prototype page from test 3 on); DEV_BUILD=ON for dev.
DEV_BUILD=${DEV_BUILD:-OFF}
# The copy has no .git: name the build after this checkout (Version label, log snapshot).
GIT=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo nogit)
[ -n "$(git -C "$SRC" status --porcelain 2>/dev/null)" ] && GIT="$GIT-dirty"
ssh "$HOST" "set -e; cd $REMOTE && cmake -S src -B build -DCMAKE_BUILD_TYPE=Release -DLTXAG_DEV_BUILD=$DEV_BUILD -DLTXAG_GIT=$GIT >/dev/null \
  && cmake --build build -j 8 2>&1 | grep -E 'error|warning' || true; \
  cd build && ctest --output-on-failure"
