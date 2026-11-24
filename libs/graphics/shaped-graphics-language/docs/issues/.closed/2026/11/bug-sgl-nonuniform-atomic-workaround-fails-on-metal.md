---
created: 2026-11-24
refs:
  - test: sg - an atomic at a nonuniform index into an SGL binding array is right in each form CHK-561 writes
  - file: libs/graphics/shaped-graphics/tests/compute/compute-sync-test.cc
closed: 2026-11-24
resolution: fixed
---

# The CHK-561 test's forced workaround counts every lane into the even element on metal

The test came with 2c784fb4c (CHK-553) and fails on an Apple M4 at c31171848, every run, alone and in the suite.
Metal reports no `nonuniform_atomic_workaround`, so only the forms the test forces reach the workaround; a program on metal never does.

Measured, 64 threads, `count_off` in `tests/shaders/sgl/binding_arrays.sgl`:

- The even element's count is 64 and the odd one's 0, where each should be 32.
- The even lanes' values before run 32 to 62, so each add saw a total of 32: one round per simdgroup, every lane in it.
- The odd elements' mins come out untouched (`s == 1` fails for every slot).
- The run stops at nexus' 30 failed checks, so the section "the atomics written by each lane" never runs: nexus reports it unreachable.

Measured 2026-11-24, each form run alone: "by a leader" fails; the native form and "by each lane" pass.

## Resolution

Apple's compiler moves the leader loop's body past the loop's exit, where the lanes reconverge.
The AIR of the emitted MSL (`xcrun metal -O2 -S -emit-llvm`) loops on `simd_broadcast_first(key) != key` alone.
`simd_sum`, `simd_prefix_exclusive_sum`, `simd_is_first` and the atomic follow the loop, so they run with all 32 lanes: a total of 32, lane 0 the only leader, its add at the even element.
A bounded loop in place of `loop:` failed the same way, which ruled out the forward-progress assumption.

`nonuniform_update_by_leader` now runs each round's subgroup operations on every lane still waiting, the others' values replaced by the operation's identity, and a lane leaves only after them.
All three forms pass on the M4, 607 checks; shaped-graphics-test and shaped-graphics-language-test pass whole.
Intel and AMD on dx12 and vulkan, where the workaround is what a program runs, are not run on the rewritten form yet.
