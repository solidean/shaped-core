---
created: 2026-10-17
tags: [bytes]
refs:
  - file: libs/data/babel-data/src/babel-data/data/base64.hh
---

# A byte-level base64 codec in clean-core

`babel::base64` exists and is text-oriented: it takes a `cc::string_view`, tolerates whitespace and both alphabets, and returns a `cc::string`.
A byte-level codec belongs in clean-core's `bytes/` instead, and moving babel's would drag its tolerant-input contract down with it — the two want different answers on malformed input.

Blocked on somebody below babel needing it; nobody does yet.
