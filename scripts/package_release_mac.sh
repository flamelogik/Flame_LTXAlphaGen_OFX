#!/usr/bin/env bash
# The macOS release tarball: the same script as Linux (package_release.sh), kept for the old call.
#   scripts/package_release_mac.sh                              on this Mac
#   HOST=<ssh host> REMOTE=<folder on the Mac> [CMAKE=...] [CMAKE_ARGS=...] scripts/package_release_mac.sh
[ -z "${HOST:-}" ] || : "${REMOTE:?set REMOTE to the build folder on the Mac}"
exec "$(dirname "$0")/package_release.sh"
