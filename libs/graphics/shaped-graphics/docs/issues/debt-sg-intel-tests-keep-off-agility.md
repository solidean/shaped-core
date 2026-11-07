---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_runtime.cc
  - file: libs/graphics/shaped-graphics/tests/backends/dx12-agility-process.cc
  - file: docs/bugs-external/intel-agility-process-recompiles-acceleration-structure-kernels/readme.md
---

# On Intel, test processes keep to the system runtime, and the Agility SDK's features are tested in a process of their own

On Intel's driver, a process that created the Agility SDK's device factory compiles every shader anew, so its first acceleration-structure build costs 20–30 s.
[The external entry](../../../../../docs/bugs-external/intel-agility-process-recompiles-acceleration-structure-kernels/readme.md) has the reproduction.

The workaround, marked `TEMPORARY(debt-sg-intel-tests-keep-off-agility)`:

- `dx12::choose_test_d3d12_runtime()` picks the system runtime when the first hardware adapter is Intel's and `SC_DX12_RUNTIME` names none.
  Every test binary that creates dx12 devices calls it from its `NX_TEST_SETUP`.
- `shaped-graphics-test`'s launcher test starts the binary again with `SC_DX12_RUNTIME=agility`, running only the invocables listed in `dx12-agility-process.cc` on two Agility drivers, one per route.
  Its results land beside the binary as `shaped-graphics-test.exe.agility.results.xml`.

What it costs on Intel, where the default run used to reach these paths:

- Only the listed sg invocables reach the Agility SDK's paths: Shader Model 6.9, shader execution reordering and opacity micromaps.
  An invocable that comes to need them and is not added there has no Intel run of that path.
- sr's micromap bake and sv's tracer run on the system runtime there.
  The Arc B580 reports no opacity micromaps on either API, so nothing was lost on that machine; an Intel GPU that has them on dx12 would.
- `sg dx12 - devices come from the Agility SDK exactly where it was fetched and asked for` skips in `shaped-graphics-dx12-test`.
  The Agility drivers fail instead when the SDK did not load in the process started for it.

Remove it when Intel's driver reads its cache back in a process that loaded the Agility SDK: `run.py` in the external entry exits 0.
