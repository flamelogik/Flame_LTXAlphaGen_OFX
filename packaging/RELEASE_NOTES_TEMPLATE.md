> **Pre-release, built by CI, not yet verified on real hardware.** These packages were built and
> smoke-tested (unit tests and the system test's fast tier, without a GPU or the model) by GitHub
> Actions from commit `@COMMIT@`. A maintainer still has to run the full system test with the real
> model on Linux and macOS; the release is promoted once both pass (CONTRIBUTING.md, "Releases").

LTX Alpha Gen @VERSION@ (`@TAG@`)

## Download

- **Linux:** `LTXAlphaGen-@VERSION@-rocky9.tar.gz`. Built on Rocky Linux 9 for Flame 2027.x.
  SHA-256: `@SHA_LINUX@`
- **macOS (Apple Silicon):** `LTXAlphaGen-@VERSION@-macos-arm64.tar.gz`. Built for macOS 15 or
  later on Apple Silicon, for Flame 2027.x. Signed ad hoc only (not notarised).
  SHA-256: `@SHA_MAC@`

```
tar xzf LTXAlphaGen-@VERSION@-rocky9.tar.gz     # or LTXAlphaGen-@VERSION@-macos-arm64.tar.gz
cd LTXAlphaGen-@VERSION@
sudo ./install.sh
```

Upgrading: quit Flame, run `install.sh` again, then start Flame. Your models, settings and cached
mattes are kept. First install: run Setup on the node's Setup tab (see README.md).

You download the LTX model weights yourself and are responsible for your own LTX licence.

Build log: @RUN_URL@
