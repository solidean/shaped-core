---
created: 2026-11-26
priority: next
refs:
  - file: libs/graphics/shaped-rendering/tests/mix-test.cc:110
  - test: sr - a mix reaches both of its endpoints exactly
---

# A mix at weight 1 writes the image it replaces, not the source

`sr - a mix reaches both of its endpoints exactly` fails on every backend of an RTX 5080 (driver 32.0.16.1088), at `origin/main` 4afba6c34 plus two unrelated commits.
At weight 1 the mix writes the image it replaces rather than the source: `replaced[0][c] == source[c] => 0.33333334 == 0.6666667`, at `mix-test.cc:110`, for `pixel_format 34`.
It fails the same on dx12 (hardware and native DXIL) and on vulkan (with and without native SPIR-V), and it fails alone, so no other test's state reaches it.

It arrived with e874302ba (a mix at weight 1 writes the source itself), which changed exactly this behavior.

```bash
uv run dev.py test "sr - a mix reaches both of its endpoints exactly" --no-server
```

The full run that found it used seed 3942882140505140765; the single test above fails without it.
