---
title: LOD bias across dx12 and vulkan
status: active
created: 2026-11-09
updated: 2026-11-12
---

# LOD bias across dx12 and vulkan

## Goal

The same sampler, on the same NVIDIA GPU, selects a different mip level on dx12 than on vulkan once anisotropic filtering is on.
A previous session estimated the gap at about 0.25 LOD.
Done means four things:
images that show the gap to someone who has never heard of it,
a measurement that states it as a number per path and per anisotropy,
a small distributable tool that reports it for every adapter on a machine,
and a backend-dependent correction inside sg, so rendering code written against sg and SGL samples the same LOD everywhere.

## Context

- `sg::sampler::mip_lod_bias` and `max_anisotropy` passed straight through to `MipLODBias` / `mipLodBias`, so any gap is the driver's.
  The translations are [dx12_sampler.cc](../../libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_sampler.cc)
  and [vulkan_sampler.cc](../../libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_sampler.cc).
- dx12 picks an adapter by LUID (`dx12_config::adapter_luid`); vulkan reports a LUID and takes `choose_physical_device`, so one GPU can be matched across both APIs.
- SGL has `sample(…, bias =)`, `level =`, `grad_x/grad_y =` and `ddx/ddy`.
- The fix (P4) is designed from the measurement (P2), since which sampling paths are off decides where a correction can live.

## Findings

Measured 2026-11-09 on an RTX 5080 (dx12 driver 32.0.16.1088, vulkan driver 2559967232), with an AMD Radeon iGPU as the control.

- **The AMD iGPU shows no shift**: dx12 and vulkan cross the half level within 0.002 LOD on every path.
  Their shapes differ, though: AMD's vulkan sharpens the blend around the half level like NVIDIA's dx12, and its dx12 stays linear.
  The half-level metric is blind to that by design, and a bias cannot change a slope.
- **NVIDIA dx12 matches the specification** to about +0.02 LOD.
- **NVIDIA vulkan picks a coarser level once anisotropy is on**: +0.164 at 2x, +0.213 at 4x, +0.250 at 8x and 16x, and 0 with anisotropy off.
- **The gap does not depend on the bias, the path or the footprint**.
  Sampler bias, `sample(bias)`, explicit gradients and even an explicit `level` all shift alike, and an isotropic footprint shifts as much as a 16:1 one.
- **Its cause is the shape of the blend, not a level bias**.
  With anisotropy on, NVIDIA sharpens the trilinear blend on both APIs, to a slope s of 1.5 / 1.75 / 2 (blend width 0.533 / 0.458 / 0.400 against a linear 0.800).
  dx12 centres that ramp on the half level; vulkan starts it at the integer.
  The shift is then exactly (1 - 1/s) / 2 = 1/6, 3/14 and 1/4, which is what was measured.
- **So a sampler LOD bias of -(1 - 1/s) / 2 on vulkan makes the two curves coincide**, and since every path takes the sampler's bias, one correction in sg's vulkan sampler translation covers them all.
- On the floor, the gap averages -0.276 LOD at 16x, and 0 with anisotropy off.

## P1: Probe tool skeleton

- [x] P1.1 Replace the half-rewritten examples/graphics/mip-bias with a nexus tool binary `lod-probe` under shaped-rendering
  - Files: libs/graphics/shaped-rendering/tools/lod-probe/
  - Note: examples/graphics/mip-bias/ is unhooked from examples/CMakeLists.txt; deleting the directory is left to the user
- [x] P1.2 Enumerate adapters on dx12 and vulkan and pair them by LUID
  - Outcome: `sg::enumerate_dx12_adapters()`, `sg::enumerate_vulkan_adapters()` and `vulkan_config::adapter_luid` added to sg
- [x] P1.3 Create a context per (adapter, backend) and run one body on each

## P2: Measurement

- [x] P2.1 SGL probe shader: affine-UV tiles over an r16_float texture whose mip i holds i
- [x] P2.2 Tile grid: minor-axis LOD x anisotropy ratio x angle, expected LOD computed on the CPU
  - Deviation: rows of footprint shapes, each sweeping the LOD in 1/64 steps (X1)
- [x] P2.3 Measure the four paths: no bias, sampler bias, shader bias, explicit gradients
  - Note: an explicit-level path was added as a fifth
- [x] P2.4 Report: offset table per backend and dx12 minus vulkan, as text and json
- [x] P2.5 Run on the local RTX 5080 and record the numbers
  - Outcome: see Findings

## P3: Images

- [x] P3.1 Perspective floor rendered on both backends with a detail texture
- [x] P3.2 LOD heatmap with contour lines, and both backends' contours overlaid in one image
- [x] P3.3 Side-by-side and amplified difference images
  - Outcome: `lod-probe --images` writes a 3x2 figure per sampler setting, `<gpu>-floor-<setting>-dx12-vs-vulkan.png`

## P4: Unification in sg

- [x] P4.1 Design the correction from the P2 data (where it lives, what it is keyed on, how it is overridden)
  - Outcome: D3, D4
- [x] P4.2 Implement it in sg (and SGL emission if shader-side paths are affected)
  - Outcome: `anisotropic_lod_correction` in vulkan_sampler.cc, applied in `to_vk_sampler_info`; no SGL change needed, since every path takes the sampler's bias
  - Note: lod-probe now measures vulkan twice, raw and corrected, and `dx12 minus vulkan+sg` reads 0.000 to 0.001 everywhere
- [x] P4.3 GPU test: dx12 and vulkan agree within tolerance after correction
  - Outcome: `sg - an anisotropic sampler is half way between two mips at the half level, its bias included` in sampler-test.cc; verified to fail with the correction off
- [x] P4.4 Docs and cheat sheet
  - Files: shaped-graphics docs/concepts/anisotropic-lod.md, cheat-sheet.md, sampler.hh, CLAUDE.md

## P5: Distribution

- [x] P5.1 Package the tool with its DLLs and a readme for others to run
  - Outcome: `build/lod-probe.zip` (exe, SDL3, dxcompiler, dxil, readme) runs from an unzipped folder outside the repo
  - Files: tools/lod-probe/package.py, tools/lod-probe/readme.md
- [x] P5.2 Report format others can send back
  - Outcome: `lod-probe-<gpu>.txt` and `.json` per GPU; the readme asks for both

## P6: Standalone probe for the blog post

- [x] P6.1 Repo skeleton `lod-check` next to shaped-core, unpublished: CMake, MIT license, readme
  - Outcome: c:/Users/Julius/projects/lod-check, one local commit, no remote
- [x] P6.2 Shared measurement and report: the sweep, half-level crossings against the spec, a paste-ready markdown block, no network code
  - Note: a compute dispatch with explicit levels and gradients (D7); the 3x finding below is reported as one line, not a table
- [x] P6.3 D3D12 backend, compiling its HLSL at run time through the d3dcompiler that ships with Windows
- [~] P6.4 Vulkan backend, loading the loader at run time, with committed SPIR-V
  - Note: runs on Windows; the Linux arm (dlopen, os-release) is unbuilt until CI or a Linux machine
- [x] P6.5 Metal backend, compiling its MSL at run time
  - Outcome: built without warnings on an M4; no shift, linear blend at every anisotropy
- [x] P6.6 Validate against lod-probe on the RTX 5080 and the Radeon iGPU
  - Outcome: NVIDIA vulkan +0.161 / +0.213 / +0.247 against dx12 ~0, AMD 0 on both with vulkan widths 0.64 / 0.53 / 0.40 — lod-probe's numbers
- [~] P6.7 Release builds in CI for Windows x64, Linux x64 and macOS arm64
  - Note: .github/workflows/build.yml written (macOS universal, Linux on 22.04 with gcc 13); unrun until the repo exists

## P7: lod-probe on metal

- [ ] P7.1 A metal path in shaped-core's lod-probe, so the Mac renders the images too

## P8: Blog post

- [ ] P8.1 Collect results: the Mac, and colleagues' machines
  - Note: the GitHub repo, its Results discussions and the Mastodon account wait on colleagues' agreement
- [ ] P8.2 Draft the post on solidean.com/blog, with sliders and links to both tools
  - Note: includes an aside on 3x anisotropy applied as 2

## Decisions

### D1: Affine-UV tiles instead of a perspective floor for measuring (2026-11-09) [P2.1, P2.2]
- **Context:** A perspective floor's derivatives vary per pixel and depend on coarse vs fine derivatives, so its expected LOD is only approximate.
- **Decision:** Measure on screen-aligned tiles whose UV is an exact affine function of the pixel position, so each tile has a known footprint and a known expected LOD.
  The floor is kept for the images only.
- **Why:** The measured value per tile is then a constant, and `measured - expected - bias` is the driver's offset with nothing of ours in it.
- **Alternatives:** Derivatives in the shader (`ddx`/`ddy`), which carry the coarse/fine ambiguity into the reference.

### D2: The tool lives in shaped-rendering as a nexus binary (2026-11-09) [P1.1]
- **Context:** It needs sg on two backends, slib for SGL, and PNG writing; it is meant to be handed to others.
- **Decision:** `libs/graphics/shaped-rendering/tools/lod-probe/`, a nexus binary with an `ASYNC_COMMAND` default entry, modelled on shaped-view.
- **Why:** shaped-rendering already depends on all three, and a nexus binary carries its own tests beside the command.
- **Alternatives:** An example (examples are named `*-example` and not meant for distribution); repo-root `tools/` (repo dev tools only).

### D3: Correct vulkan towards dx12, in the sampler translation (2026-11-09) [P4.1, P4.2]
- **Context:** NVIDIA's dx12 matches the specification within 0.02 and centres its sharpened blend; its vulkan samples (1 - 1/s) / 2 levels too coarse.
- **Decision:** dx12's level is sg's reference; the vulkan backend adds the lag's negative to `mipLodBias` of every anisotropic sampler, in `to_vk_sampler_info`.
- **Why:** Every path, the explicit level included, takes the sampler's bias, so one place covers dynamic, static and SGL samplers, and shaders stay untouched.
- **Alternatives:** A bias added in SGL's vulkan emission (misses explicit levels and every non-SGL shader); correcting dx12 towards vulkan (moves the backend that matches the specification).

### D4: A measured table keyed on vendor and anisotropy, not a calibration at run time (2026-11-09) [P4.1]
- **Context:** The user asked for a manual, backend-dependent mechanism; the shift is a step function of floor(log2(anisotropy)) on NVIDIA and 0 on AMD.
- **Decision:** `anisotropic_lod_correction(adapter, anisotropy)` returns -1/6, -3/14 or -1/4 on NVIDIA and 0 elsewhere; `vulkan_config::correct_anisotropic_lod` turns it off.
- **Why:** No start-up cost and nothing to cache, and lod-probe reports a new driver that differs as a nonzero `dx12 minus vulkan+sg` row.
- **Alternatives:** Running the probe at context creation (start-up cost, and a GPU readback before the first frame).

### D5: The blog's tool is a standalone repo with no dependencies, beside lod-probe (2026-11-11) [P6.1, P6.2]
- **Context:** Readers should check the finding on their own GPU, and also meet our stack.
- **Decision:** `lod-check` is plain D3D12, Vulkan and Metal in a few files, reporting numbers against the spec; lod-probe stays the in-stack version with images.
- **Why:** A probe readable in one sitting is one people trust, and comparing against the spec gives a single-API machine (a Mac) a result.
- **Alternatives:** Shipping lod-probe alone, which needs most of shaped-core to build from source.

### D6: lod-check never touches the network (2026-11-11) [P6.2]
- **Context:** Results should be shared, but only by a deliberate act.
- **Decision:** It prints a markdown block and writes it to a local file, holding no hostname, user name or LUID; at most it prints the Discussions link.
- **Why:** People rightly distrust tools that phone home, and the source makes the claim checkable.
- **Alternatives:** An upload flag, or opening a prefilled form in the browser.

### D7: lod-check samples in a compute shader, with explicit levels and gradients only (2026-11-11) [P6.2]
- **Context:** lod-probe draws tiles and samples in a pixel shader, which needs a render target and a vertex path per API.
- **Decision:** One compute dispatch; the paths are an explicit level, a square footprint's gradients and a 4:1 footprint's gradients.
- **Why:** lod-probe showed every path shifts alike, implicit derivatives included, and a compute dispatch halves each backend.
- **Alternatives:** Porting lod-probe's draw, which would add implicit derivatives and a shader bias at about twice the code.

## Deviations

### X1: Read the level at half-level crossings of a sweep, not as a per-tile mean (2026-11-09) [P2.2, P2.4]
- **Planned:** 75 tiles at quarter-level steps, each reporting `measured - expected` as the hardware's offset.
- **Actual:** six footprint rows, each sweeping 128 levels in 1/64 steps; the offset is read where the sample crosses m + 0.5, and the blend's 10-90% width is reported beside it.
- **Why:** The first run showed NVIDIA's blend is not linear with anisotropy on, on either API, so `measured` is not the level.
  Averaging over quarter steps had cancelled most of that error by symmetry.
- **Impact:** The report gained a blend-width table; the explicit-level path takes its level per tile, from a vertex attribute.

## Log

- 2026-11-09: Plan created after mapping the sampler translation, adapter selection and SGL sampling surface.
- 2026-11-09: P1, P2 and P3 done; the first measurement forced X1, and the Findings section records the result.
- 2026-11-09: P4 done (D3, D4): the correction lands dx12 and vulkan within 0.001 LOD on the RTX 5080; P5 packaging in progress.
- 2026-11-09: P5 done; the packed zip ran standalone and reproduced both results.
- 2026-11-09: P1.2 follow-up: the images show a page of text instead of the checker, whose moiré hid the blur.
  A flat page at level 1.25, natural and with one ink colour per mip, joins the floor; the tinted page exposed AMD's differing blend shape, now in Findings.
- 2026-11-11: P6-P8 added for the blog post (D5, D6); P6.1 started.
- 2026-11-11: lod-check measures what lod-probe did on both local GPUs (P6.1-P6.3, P6.6 done; D7).
  Side findings: a max anisotropy of 3 is applied as 2 for a 4:1 footprint on both vendors and APIs, and NVIDIA dx12 sharpens 3x like 4x (width 0.46) where vulkan uses 0.53.
- 2026-11-11: anisotropic-lod.md gained dx12's 3x slope and the 3x-as-2 finding; publishing lod-check deferred until colleagues agree.
- 2026-11-11: P6.5 done on an M4: Metal centres and blends as the specification says, so dx12 stays the reference.
  Its 4:1 footprint at 3x reads -0.410, which fits 3x applied as 4 where NVIDIA and AMD apply it as 2; anisotropic-lod.md records both.
- 2026-11-12: lod-probe's `--images` gained a floor with one colour per mip, and its level maps carry their half-level contours.
  Both feed P8.2 (Draft the blog post), which lives on the website's jn/blog/anisotropic-mip-blend branch.
