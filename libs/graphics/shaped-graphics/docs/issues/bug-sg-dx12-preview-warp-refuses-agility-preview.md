---
created: 2026-11-11
refs:
  - file: libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md
  - file: extern/agility-preview/dependency.yml
---

# The preview WARP creates no device under the preview Agility runtime

WARP `1.65535.22-preview` is the one software adapter that implements DXR's clustered geometry and partitioned TLAS.
Under the preview Agility SDK, the runtime those features need, it creates no device at all.
So the preview build tests on the WARP it had, which lacks them, and nothing here pins the preview WARP.

What was bisected on 2026-11-11, with Windows Developer Mode on:

- `ID3D12DeviceFactory::CreateDevice` on the WARP adapter returns `E_INVALIDARG` with Agility `1.722.1-preview` and with `1.722.0-preview`.
- It does so with both experimental features enabled, with `D3D12RaytracingPreview` alone, and with none.
- It does so with the DLL staged beside the executable, and with a copy beside the preview `D3D12Core.dll` too.
- The same DLL creates its device on the system runtime (`SC_DX12_RUNTIME=system`).
- The system WARP creates its device on the preview runtime.

So the fault is the pairing, not either half alone.
The next WARP preview or Agility preview is the thing to retry, with `SC_DX12_ADAPTER=warp` and a preview build's dx12 tests.
Pinning it again is a new `extern/warp-preview`, staged by a build configured with `SC_AGILITY_PREVIEW` in place of `extern/warp`'s.
