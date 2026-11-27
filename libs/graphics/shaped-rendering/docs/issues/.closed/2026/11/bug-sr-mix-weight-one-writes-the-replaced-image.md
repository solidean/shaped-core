---
created: 2026-11-26
priority: next
refs:
  - file: libs/graphics/shaped-rendering/tests/mix-test.cc:110
  - test: sr - a mix reaches both of its endpoints exactly
closed: 2026-11-27
resolution: fixed
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

## Resolution

The shader was right; the test's source sat on a rounding tie.
Format 34 is `rgb10a2_unorm`, and the source's alpha of 0.5 is 1.5 steps of its 2 bits.
The RTX 5080 stores that tie as 1 (0.333), and `sr_test::quantized` rounds it to 2 (0.667); D3D and Vulkan leave a tie's direction to the implementation.
The weight-1 select did write the source: 0.333 is not the destination's alpha of 1.0, it is the source's rounded down.
Before e874302ba the check allowed a step, which hid the tie; the source's alpha is now 0.625, on no tie at any width the sweep holds.
Not re-run on an NVIDIA GPU; the Arc B580 passes as before.
