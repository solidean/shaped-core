---
created: 2026-10-31
refs:
  - test: sg - a pixel stage returns one color without a struct, or nothing and writes memory alone
---

# A rendering scope with no color and no depth target

An SGL pixel stage may return nothing and write memory alone (CHK-418), so its pipeline has no color target.
Every backend asks a rendering scope for at least one color or depth-stencil target, so such a draw needs a depth target it never reads.
bgfx's deferred example clears and lights its g-buffer that way, through image stores into a framebuffer with nothing bound.

- **Wanted**: `render_to` with neither, where the backend can: the scope then states its extent, and rasterizes against it.
- **D3D12** draws with no RTV and no DSV, from the viewport and the scissor alone.
- **Vulkan** dynamic rendering takes a render area with no attachment.
- **Metal** takes a render pass descriptor with no attachment once it states `renderTargetWidth`, `renderTargetHeight` and a default sample count.
- **WebGPU** refuses a render pass with no attachment, so it would be a feature, or a hidden attachment sg creates.
- Today's workaround is a depth target the pipeline names and no test reads, which the GPU test above uses.
