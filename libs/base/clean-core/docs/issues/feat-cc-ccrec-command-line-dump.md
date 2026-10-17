---
created: 2026-10-17
tags: [rec, dev.py]
refs:
  - file: libs/base/clean-core/src/clean-core/record/serialize.hh
  - symbol: cc::rec::load_recording
---

# There is no way to read a `.ccrec` from the command line

Diagnosing the `cc::symbolizer` race (fixed since) meant scraping printable runs out of the file with a throwaway script.
`cc::rec::load_recording` is the whole reader and nothing exposes it.

A `dev.py` subcommand that dumps a recording's events is what would make the sidecar pay off the way `nx::config::recorded` intends.
