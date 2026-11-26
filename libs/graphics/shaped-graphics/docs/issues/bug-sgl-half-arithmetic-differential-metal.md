---
created: 2026-11-26
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/differential-test.cc:436
---

# half_arithmetic.sgl's half_operations disagree with the interpreter on metal at a precise floor

On an Apple M4, relwithdebinfo, shaped-graphics-test's differential test fails on `half_operations` in `half_arithmetic.sgl`, metal's msl at a precise floor.
4 of 1024 output elements lie beyond relative 0.001 of the interpreter's value:

```text
[115] -0.1361084 (0xbe0b6000) against -0.140625 (0xbe100000)
[299] 0.21289062 (0x3e5a0000) against 0.2109375 (0x3e580000)
[371] -1.4970703 (0xbfbfa000) against -1.5 (0xbfc00000)
[643] -2.4570312 (0xc01d4000) against -2.453125 (0xc01d0000)
```

Seen on 2026-11-26 at main's tip after a07d8550d, in the pre-push gate of an unrelated metal residency change, b66d6a723.
The test and the half-precision rules it checks changed the same day and the day before (a07d8550d, 67f74f859, bc78b9d89, 994ab100e).
Not yet run at the commit before those, so whether one of them brought it in is a hypothesis.
The GPU values carry more bits than a half holds (0xbe0b6000 is not a half's rounding), which reads as a half kept at float precision on metal.

