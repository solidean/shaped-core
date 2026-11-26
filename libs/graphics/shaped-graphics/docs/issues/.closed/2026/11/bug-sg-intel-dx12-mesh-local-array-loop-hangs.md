---
created: 2026-11-26
refs:
  - file: docs/bugs-external/dxc-pixel-signature-packs-primitive-into-vertex-row/shaders.hlsl
closed: 2026-11-26
resolution: fixed
---

# Intel's dx12 driver hangs the device on a mesh stage that writes its vertices from a loop over a local array

Found on 2026-11-26 while reducing the per-primitive repro, on the Arc B580, dx12 driver 32.0.101.9150, and not investigated.
A mesh stage that writes its four vertices as below hangs the device: `GetDeviceRemovedReason` is `DXGI_ERROR_DEVICE_HUNG` after the draw.

```hlsl
float2 const corners[4] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, 1)};
for (uint v = 0; v < 4; ++v)
    verts[v].position = float4(corners[v], 0.5, 1.0);
```

The same stage writing each vertex's position as its own statement draws right, and so do the AMD iGPU and WARP with the loop.
The pixel stage and the per-primitive attributes made no difference: the variant with a linear vertex attribute hung too.
Compiled by the Windows SDK's dxc 1.9.2602.17 at `-O3`.

Not known: whether SGL ever writes this shape for a mesh stage, whether DXC's DXIL keeps the array as memory or the driver mishandles it, and whether a compute stage hangs too.
Putting the loop back in `shaders.hlsl`'s `ms` runs it in this host:
[dxc-pixel-signature-packs-primitive-into-vertex-row](../../../../../../../../docs/bugs-external/dxc-pixel-signature-packs-primitive-into-vertex-row/readme.md).

## Log

**2026-11-26: Intel's driver, reproduced standalone; sg's empty root signatures carry one unread constant.**
It is not the loop: a mesh stage that reads a constant table at a dynamic index hangs the device under a root signature of no parameter.
DXC and SGL both state such a table as an `internal constant`, and one 32-bit root constant that nothing sets or reads avoids it.
The minimal repro is a mesh stage alone, one vertex and no triangle.
[intel-d3d12-mesh-table-without-root-parameters-hangs](../../../../../../../../docs/bugs-external/intel-d3d12-mesh-table-without-root-parameters-hangs/readme.md) holds it.
sg's layouts are empty only where shader diagnostics are compiled out, and its frozen build there did not hang on the same container; the readme lists what that rules out.
The mixed-route test's `tabled_ms` and `tabled_ts` read tables on every route now.
