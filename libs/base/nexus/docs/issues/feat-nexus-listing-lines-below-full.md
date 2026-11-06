---
created: 2026-11-06
priority: someday
refs:
  - file: libs/base/nexus/src/nexus/tests/export/listing_json.cc
  - file: libs/base/nexus/src/nexus/tests/export/catch2.cc
---

# nexus listings name line 0 for a test below SC_SOURCE_SITES=FULL

Below FULL a declaration keeps its file and the code address of its body, not a line, so --list-tests-json, the Catch2 listing and the XML reporter write line 0.

An IDE's test explorer then opens the right file at its top; the failure output is unaffected, since `dev.py test` resolves a test's site to its `TEST` line.
An alias named after no test also has no file below FULL, so a file filter does not reach it.
Hypothesis: resolving the body's address in-process with `cc::symbolizer` at listing time would give the line back, at the cost of loading debug info for a listing.
