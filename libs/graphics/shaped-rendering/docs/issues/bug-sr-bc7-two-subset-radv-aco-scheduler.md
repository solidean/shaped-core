---
created: 2026-11-25
tags: [vulkan, radv, driver, bc7]
refs:
  - test: sr - BC7 compression reproduces every two-subset partition and holds a gradient
  - file: libs/graphics/shaped-rendering/shaders/compression/bc.sgl
  - symbol: sr::bc_compress_routine
---

# BC7 garbles five partition blocks on RADV's DXC route, and only with ACO's scheduler on

On an AMD Steam Machine (RADV NAVI33, Mesa 26.2.0-devel git-5c65d4fc31, SteamOS), `sr vulkan / vulkan` fails the partition case every run: max error 121, PSNR 27 dB.
`sr vulkan - native-spirv` passes the same test on the same device, so it is the SPIR-V DXC writes from SGL's HLSL that goes wrong, not the encoder's logic.

Measured:
- Exactly blocks 19, 27, 35, 43 and 51 of `partition_ramps` fail, one block column (`p % 8 == 3`), rows 2 to 6.
- Their mode 3 endpoints are right, and in all five subset 1's endpoints were swapped, so the index flip of `bc.sgl`'s `encode_two_subsets` ran.
  Many other blocks take that flip and pass.
- The indices of both subsets are wrong: subset 0's read as 0 / 2 / 3 where its ramp is 0, 1, 2, 1, …, 3.
- The emitted `hlsl-vulkan` of that flip reads correctly.
- With `RADV_DEBUG=nocache ACO_DEBUG=nosched` the test passes five runs of five, and every `sr - BC` test with it.
  `ACO_DEBUG=novn` still fails the same five blocks; `ACO_DEBUG=noopt` fails those and blocks 3 and 11.
- This RADV build has no LLVM backend (`RADV_DEBUG=llvm` aborts), and no lavapipe is installed, so no second compiler was tried.

The cause is in ACO, found and minimized: [docs/bugs-external/radv-aco-scheduler-wrong-bc7-indices](../../../../../docs/bugs-external/radv-aco-scheduler-wrong-bc7-indices/readme.md).
ACO spills VGPRs to LDS with `ds_*_addtid` instructions that carry no memory sync info, so its pre-RA scheduler reorders a spill and a reload of the same LDS slot.
In this encoder a reload inside the partition-anchor table lookup was hoisted above its own spill, so the lanes of one block column read a stale anchor.
A 24-line integer shader in that entry reproduces it with an exact CPU check, and has no undefined behavior to expose.

`sr - BC1 and BC7 compress any mip level, a partial block and a single texel included` trips over it too, intermittently and only within the whole binary; `nosched` passes it.
A stale LDS slot explains that: LDS outlives a workgroup, so what ran before decides what the reload reads.
Both skip on that driver version, on the DXC route alone, through `sg::known_issue_affecting`.
Next: report it to Mesa with that entry.
