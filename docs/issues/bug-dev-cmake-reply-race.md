---
created: 2026-11-11
tags: [dev]
---

# A CMake file-api reply file vanishes under dev.py

Several times on 2026-11-11, dev.py's target discovery failed with FileNotFoundError on build/<preset>/.cmake/api/v1/reply/target-babel-*-*.json right after a configure.
A second configure of the same build dir, likely the editor's CMake integration, seems to replace the reply set underneath it; touching a CMakeLists and building again recovers.
dev.py could retry discovery once, or re-read the index, when a referenced reply file is missing.
