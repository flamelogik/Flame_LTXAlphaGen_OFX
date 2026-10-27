# Contributing

Thanks for helping with LTX Alpha Gen for Flame. This page covers building, testing and how a
release is made. User documentation is in [README.md](README.md).

## Before you change things

- Design notes live next to the code: [CLAUDE.md](CLAUDE.md) is the overview and the index of
  numbered decisions; `ofx/CLAUDE.md`, `worker/CLAUDE.md`, `helper/CLAUDE.md` and
  `flame_hook/CLAUDE.md` hold the detail. Code comments refer to decisions by number
  ("decision 34"); keep the numbers stable and add new decisions with the next free number.
- Never commit LTX code or model weights. Each user downloads them under their own LTX licence.

## Build

- **Linux release builds must be made on Rocky Linux 9** (glibc 2.34, GCC 11): binaries from
  newer distributions don't load in Flame. Any Linux works for a test build:
  `cmake -S . -B build -DWORKER_TEST_PYTHON=<python with numpy> && cmake --build build -j`.
  A `rockylinux/rockylinux:9` container works too (see `.github/workflows/release.yml` for the
  packages).
- **macOS** (Apple Silicon, Xcode or the Command Line Tools, cmake): the same commands. Known SDK
  workarounds are in CLAUDE.md ("Building, testing, releasing").
- `-DLTXAG_DEV_BUILD=ON` (the default) adds the Prototype page and the dummy worker; release
  builds use `OFF`.
- A release tarball on this machine: `scripts/package_release.sh` (release build, ctest, package,
  `.sha256` into `dist/`). `HOST=<ssh host> scripts/package_release.sh` does the same on another
  machine over ssh.

## Test

- **Unit tests:** `cd build && ctest --output-on-failure` (9 tests, no host or GPU needed).
- **System test, fast tier** (a Flame-like OFX host drives the real plugin with a dummy worker;
  about a minute, no GPU, model or Setup): `scripts/maintainer_test.sh --dev`. On macOS pass a
  python with numpy: `JOB_PYTHON=<python> CMAKE_ARGS=-DWORKER_TEST_PYTHON=<python> scripts/maintainer_test.sh --dev`.
  Run it after every feature or fix, on both platforms if you can.
- **Full tier** (real model; needs a GPU machine or an Apple Silicon Mac with a completed Setup):
  see [systest/README.md](systest/README.md).
- Anything that touches the UI or Flame's behaviour still needs a check in Flame itself.

## What CI does

`.github/workflows/release.yml` runs on every pushed tag `v*` (and by hand, as a dry run without
publishing). On Rocky 9 (container) and macOS arm64 it checks that the tag matches
`LTXAG_VERSION` in `CMakeLists.txt`, builds the release bundle, runs ctest, runs the system
test's fast tier on a dev build and packages `LTXAlphaGen-<version>-rocky9.tar.gz` and
`-macos-arm64.tar.gz` with `.sha256` files. If both platforms pass, it publishes a GitHub
**pre-release**. Hosted runners have no GPU: the real model is only tested by maintainers.

## Releases (maintainers)

1. Bump `LTXAG_VERSION` in `CMakeLists.txt` (and its history comment), commit, then tag and push:
   `git tag -a v0.2.2 -m "v0.2.2" && git push origin v0.2.2`.
2. Wait for the release workflow; it creates the pre-release `v0.2.2`.
3. On a Linux GPU machine and on an Apple Silicon Mac (each with Setup completed for your user,
   nothing else using the GPU):
   ```
   git fetch --tags && git checkout v0.2.2
   scripts/maintainer_test.sh v0.2.2
   gh release upload v0.2.2 ltxag-test-linux-v0.2.2.json     # or ltxag-test-macos-v0.2.2.json
   ```
   `maintainer_test.sh` downloads the platform's tarball, checks its sha256, runs the full tier
   on the packaged bundle and writes the result file. Without `gh` on that machine, download the
   tarball by hand and run `scripts/maintainer_test.sh LTXAlphaGen-0.2.2-rocky9.tar.gz --sha256 <hash>`.
4. Actions > promote > Run workflow, tag `v0.2.2` (tick `dry_run` to only check), or
   `gh workflow run promote.yml -f tag=v0.2.2`. It requires a PASS result for both platforms that
   matches the release's tarballs, then clears the pre-release flag and marks it Latest.
5. A failed hardware test: fix, bump the version (or tag `v0.2.2-rc2` on the fixed commit) and
   start again. Edit the release notes on GitHub if you want to add what's new.

## Licence

The project is licensed under the [Mozilla Public License 2.0](LICENSE); see [NOTICE](NOTICE) for
the copyright and third-party code (the vendored OpenFX headers keep their BSD licence). By
contributing you agree that your contribution is licensed under MPL 2.0.

## Security

Please report security problems privately, as described in [SECURITY.md](SECURITY.md), not in a
public issue.
