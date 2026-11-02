---
created: 2026-10-17
tags: [rec]
refs:
  - file: libs/base/clean-core/src/clean-core/record/sampling.cc
---

# A stack dictionary that retention understands

Samples write their frames out in full, so a kilohertz across many uninstrumented threads is megabytes a second of repeated return addresses.

A sampler-local intern table was tried and removed.
Definitions are written once at the front of a capture, and a bounded ring evicts oldest-first, so the ring outlives the stacks it refers to.

Whatever replaces it has to be a property of the stream rather than of the sampler.
A dictionary retention keeps, or re-emits, so that any surviving window is self-contained.
