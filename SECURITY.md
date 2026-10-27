# Security Policy

## Reporting a vulnerability

Please do not open a public issue for a security problem.

- Use GitHub's private vulnerability reporting (the "Report a vulnerability" button under this
  repository's Security tab), or
- email **conduct@logik.tv** with "LTX Alpha Gen security" in the subject.

Include the plugin version (shown on the plugin's first tab and in the log), your platform, and
steps to reproduce. We aim to acknowledge a report within a week.

## Scope

This repository holds the OFX plugin, its Python worker, Setup and the Flame hook. It does not
contain the LTX model weights or the LTX code, which are downloaded by each user under their own
licence from Lightricks; report problems in those to Lightricks.

Things worth reporting: the installer or Setup writing outside the documented folders, handling of
the Hugging Face token (it is meant to be used once and never logged), unsafe handling of files in
the staging folder or cache, and anything that lets one local user run code as another.

## Supported versions

Fixes go into the latest release only.
