---
created: 2026-11-07
refs:
  - file: docs/bugs-external/intel-agility-process-recompiles-acceleration-structure-kernels/readme.md
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_runtime.cc
closed: 2026-11-07
resolution: fixed
---

# A process on the Agility SDK pays a 20-30 s compile at its first acceleration-structure build on Intel, so the test suite takes 182 s

On Intel's driver 32.0.101.9150, a process that has called the Agility SDK's `CreateDeviceFactory` never reads the driver's acceleration-structure compile back from its cache.
Its first build, of any kind, costs 20–30 s on dx12 and on vulkan alike; every later build of the process costs nothing.
Without the Agility SDK the compile is cached per application, and the first build takes about 12 ms.
[The external entry](../../../../../../../../docs/bugs-external/intel-agility-process-recompiles-acceleration-structure-kernels/readme.md) has the matrix that isolates the trigger.
Its `run.py` is a standalone reproduction.

## Measured, 2026-11-07, Arc B580

- `shaped-graphics-test` alone: 9.9 s built without the Agility SDK, and over 175 s with it.
  In `dev.py ship` it was 182 s, the whole run's critical path against a 120 s budget.
- In that run, four unrelated tests took 27–28 s each, and every other case under 2 s.
  Each was the first acceleration-structure build of its driver, or a test waiting behind one.
- A quarter shard of the binary on Agility takes 89 s, and 55% of its sampled busy time is a vulkan TLAS build waiting in the driver's compiler.
- Our part is nothing: two triangles, one driver call, 17 ms on the system runtime.

## Mitigations, for a decision

The coverage the Agility SDK buys on dx12 is shader model 6.9, shader execution reordering and opacity micromaps.
Every option below trades some of that on Intel for time, so which one is a decision rather than a fix.

1. **Keep the Agility runtime out of processes that do not need it.**
   A `dx12_config` field, or a process-wide setting, picks the system runtime.
   The default test drivers run on it, and one Agility driver runs the tests that need SM 6.9.
   That driver must be its own process to help, since the Agility device factory alone, with no dx12 device, poisons vulkan's builds too.
   sg makes that factory the first time a dx12 context asks for the Agility runtime, so the setting must keep it from being made at all.
   Nexus runs drivers in-process, so that is either a separate binary or `--thorough` only.
2. **Scope it to the vendor.** The same, applied only where the adapter is Intel's, so other machines keep full coverage in the default run.
3. **Leave it, and raise the budget.** Cheapest, and the 182 s stays.

Applications should be affected too: `shaped-view` on this driver would wait 26 s or more at its first trace build, though that was not measured.
Option 1's knob is what an application would reach for there as well.

## Landed

Option 2, chosen on 2026-11-07, and the workaround is now [debt-sg-intel-keeps-off-agility](../../../debt-sg-intel-keeps-off-agility.md).
`dx12::choose_d3d12_runtime` and `SC_DX12_RUNTIME` pick the runtime per process, and on Intel each test binary picks the system one.
`shaped-graphics-test` takes 23.6 s alone, its Agility process included, against over 175 s.
The full `dev.py test` run, one binary at a time, takes 101 s, its longest binary `shaped-viewer-test` at 38–45 s.

## Not done

- Nothing is filed with Intel yet; the external entry carries a draft report.
- Driver 32.0.101.8629, the last good one of the related IGCIT #1548, was not tried.
- Another Agility SDK version was not tried; that needs a new pin and its terms read.
