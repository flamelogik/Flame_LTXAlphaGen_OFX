#!/usr/bin/env bash
# System test on Linux and macOS in parallel (systest/README.md): syncs this checkout to each
# test machine, builds the dev and release bundles there, runs the fast tier (dev build, dummy
# worker) and the full tier (release build, real model), and collects the results here.
#   LINUX_HOST=<ssh host> [LINUX_REMOTE=<build folder>] [LINUX_WORK=<local-disk folder>]
#   MAC_HOST=<ssh host> MAC_REMOTE=<build folder> MAC_WORK=<local-disk folder>
#     [MAC_CMAKE=<cmake on the Mac>] [MAC_CMAKE_ARGS=...]
#   [TIERS="fast full"] [ONLY=test1,test2] scripts/system_test.sh
# Leave a platform's *_HOST unset to skip it. *_WORK must be on a local disk of that machine
# (the worker's Unix socket lives there); the test machines need a completed Setup for the full
# tier. Results go to dist/systest/<commit>[-dirty]/<linux|macos>/. For testing a published
# release on one machine, use scripts/maintainer_test.sh instead (systest/README.md "Releases").
set -uo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
TIERS=${TIERS:-fast full}
ONLY=${ONLY:-}
ID=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo nogit)
DIRTY=$(git -C "$SRC" status --porcelain 2>/dev/null)
[ -n "$DIRTY" ] && ID="$ID-dirty"
RESULTS="$SRC/dist/systest/$ID"
mkdir -p "$RESULTS"
echo "system test of $ID (tiers: $TIERS) -> $RESULTS"

# run_platform <name> <host> <remote build folder> <work folder> <cmake> <extra cmake args>
run_platform() {
  local name=$1 host=$2 remote=$3 work=$4 cmake=$5 cargs=$6
  local out="$RESULTS/$name" log="$RESULTS/$name.log"
  mkdir -p "$out"
  {
    echo "[$name] sync to $host:$remote/src"
    ssh "$host" "mkdir -p '$remote/src' '$work'" &&
      rsync -a --delete --exclude .git --exclude build --exclude dist --exclude docs --exclude __pycache__ \
        --exclude CLAUDE.local.md "$SRC/" "$host:$remote/src/" || { echo "[$name] sync failed"; return 1; }
    local only=""
    [ -n "$ONLY" ] && only="--only '$ONLY'"
    # Detached on the test machine (a dropped ssh connection must not stop a long real run);
    # DONE marks the end.
    ssh "$host" "rm -f '$work/DONE'; nohup '$remote/src/systest/run_all.sh' --build-root '$remote' \
      --work '$work' --tiers '$TIERS' --cmake '$cmake' --git '$ID' $only -- $cargs \
      > '$work/run_all.log' 2>&1 < /dev/null &" || { echo "[$name] launch failed"; return 1; }
    echo "[$name] running (log: $host:$work/run_all.log)"
    local t0=$SECONDS
    until ssh "$host" "test -f '$work/DONE'" 2>/dev/null; do
      if (( SECONDS - t0 > 4 * 3600 )); then echo "[$name] no result after 4 h"; return 1; fi
      sleep 30
    done
    rsync -a "$host:$work/run_all.log" "$host:$work/DONE" "$out/" 2>/dev/null
    for t in $TIERS; do rsync -a "$host:$work/out-$t" "$out/" 2>/dev/null; done
    echo "[$name] finished after $(( (SECONDS - t0) / 60 )) min:"
    sed "s/^/[$name]   /" "$out/DONE"
    local ok=1
    for t in $TIERS; do grep -qx "$t 0" "$out/DONE" || ok=0; done
    [ $ok = 1 ]
  } >"$log" 2>&1
}

PIDS=() NAMES=() LOGS=()
if [ -n "${LINUX_HOST:-}" ]; then
  run_platform linux "$LINUX_HOST" "${LINUX_REMOTE:-ClaudeCode/LTX_OFX_build/systest}" \
    "${LINUX_WORK:-${LINUX_REMOTE:-ClaudeCode/LTX_OFX_build/systest}/work}" cmake "" &
  PIDS+=($!) NAMES+=(linux) LOGS+=("$RESULTS/linux.log")
fi
if [ -n "${MAC_HOST:-}" ]; then
  run_platform macos "$MAC_HOST" "${MAC_REMOTE:?set MAC_REMOTE}" "${MAC_WORK:?set MAC_WORK}" \
    "${MAC_CMAKE:-cmake}" "${MAC_CMAKE_ARGS:-}" &
  PIDS+=($!) NAMES+=(macos) LOGS+=("$RESULTS/macos.log")
fi
[ ${#PIDS[@]} -gt 0 ] || { sed -n '2,13p' "$0" >&2; exit 2; }
# Follow both logs while they run.
tail -n +1 -F "${LOGS[@]}" 2>/dev/null &
TAIL=$!
RC=0
for i in "${!PIDS[@]}"; do wait "${PIDS[$i]}" || RC=1; done
sleep 1
kill $TAIL 2>/dev/null
for n in "${NAMES[@]}"; do
  for t in $TIERS; do
    f="$RESULTS/$n/out-$t/console.log"
    [ -f "$f" ] && echo "$n $t: $(grep -E '^ltxag_systest .* tests,' "$f" | tail -1)"
  done
done
exit $RC
