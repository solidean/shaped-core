---
created: 2026-10-17
tags: [rec]
refs:
  - file: libs/base/clean-core/src/clean-core/record/sampling.cc
  - file: libs/base/clean-core/docs/systems/recording.md
---

# The sampler sees only threads the recorder already knows

A thread joins the sampled set by recording anything at all, so a thread that never records is invisible to the profile.
Enumerating the process's OS threads instead would sample those too.

`docs/systems/recording.md` named this as a follow-up without an entry behind it; this is that entry.
