---
created: 2026-10-17
tags: [rec]
refs:
  - file: libs/base/clean-core/src/clean-core/record/sampling.cc
  - file: libs/base/clean-core/src/clean-core/record/sampling.hh
---

# Sampling above the OS timer's ceiling

A high-resolution waitable timer floors near half a millisecond on Windows — about 1.9 kHz.
Going faster means sleeping short and spinning the remainder, which burns a core for the privilege.

Worth it only for a short, deliberate capture, so it wants to be an explicit mode rather than a rate that quietly starts spinning.
