---
created: 2026-11-22
priority: next
tags: [half, vulkan, nvidia]
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_math.sgl
  - test: sg differential - every corpus entry point*
  - issue: bug-sg-diff-half-corpus-fails-on-intel
---

# half_math's halves come back unrounded on NVIDIA's vulkan

On an RTX 4090, `half_math.sgl`'s `halves` differs from its reference, from the interpreter and from dx12 on the same GPU, on vulkan's dxc and native routes alike.
1023 of 1024 elements hold a float's precision where a half's is expected: `0x3efe7280` against `0x3efe8000`, `0x3d4b8ecd` against `0x3d4b8000`.
dx12 on the same machine agrees with the reference.

Seen at `6337cd272` on a clean checkout, so it predates the change that found it.
Hypothesis: the driver keeps a half operation in float on vulkan, as Intel's dx12 elides a round trip (bug-sg-diff-half-corpus-fails-on-intel).
`4fde5ca31` allows exactly that at `.fast`, which is worth checking against the level `half_math` runs at.
