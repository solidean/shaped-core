---
created: 2026-11-24
refs:
  - test: sg - an atomic at a nonuniform index into an SGL binding array is right in each form CHK-561 writes
  - file: libs/graphics/shaped-graphics/tests/compute/compute-sync-test.cc
---

# The CHK-561 test's forced workaround counts every lane into the even element on metal

The test came with 2c784fb4c (CHK-553) and fails on an Apple M4 at c31171848, every run, alone and in the suite.
Metal reports no `nonuniform_atomic_workaround`, so only the forms the test forces reach the workaround; a program on metal never does.

Measured, 64 threads, `count_off` in `tests/shaders/sgl/binding_arrays.sgl`:

- The even element's count is 64 and the odd one's 0, where each should be 32.
- The even lanes' values before run 32 to 62, so each add saw a total of 32: one round per simdgroup, every lane in it.
- The odd elements' mins come out untouched (`s == 1` fails for every slot).
- The run stops at nexus' 30 failed checks, so the section "the atomics written by each lane" never runs: nexus reports it unreachable.

Hypothesis, unmeasured: the failing form is "by a leader".
- The native form is `tally`'s shader with the results stored, and `tally`'s test passes on metal.
- The leader's MSL is `while (true) { if (simd_broadcast_first(key) == key) { … simd_sum(1u) … break; } }`.
  A total of 32 means all 32 lanes took the branch in the first round: either Apple's compiler treats the loop's condition as uniform, or the lanes it leaves still take part in `simd_sum`.
- `sgl emit` of `binding_arrays.sgl`'s `count_off` for `msl` prints it, given `--device-feature` for `binding_arrays`, `subgroups` and `nonuniform_atomic_workaround`.

Settling it takes a run of each form alone, or the cap on failed checks raised, so the third section runs too.
