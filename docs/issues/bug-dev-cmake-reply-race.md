---
created: 2026-11-11
tags: [dev]
---

# A CMake file-api reply file vanishes under dev.py

Several times on 2026-11-11, dev.py's target discovery failed with FileNotFoundError on build/<preset>/.cmake/api/v1/reply/target-babel-*-*.json right after a configure.
A second configure of the same build dir, likely the editor's CMake integration, seems to replace the reply set underneath it; touching a CMakeLists and building again recovers.
dev.py could retry discovery once, or re-read the index, when a referenced reply file is missing.

## Log

- **2026-11-13: in ship's base worktree it blocks a push without saying why.**
  build/proof-base's reply set lost a target file after a configure that ninja's "failed recompaction" had interrupted.
  ship then built nothing at the base, read every red there as "no result", and blocked twice on 347 sv tests the base has red too.
  A `dev.py configure` in the worktree recovered it; ship should report a base that does not configure, rather than fold it into no result.
