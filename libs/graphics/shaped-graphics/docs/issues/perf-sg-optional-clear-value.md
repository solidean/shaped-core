---
created: 2026-10-17
tags: [dx12]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_expected_messages.hh
  - symbol: sg::texture_description
---

# An optional clear value on `texture_description`

D3D12 takes a `D3D12_CLEAR_VALUE` at resource creation and uses it to pick a fast-clear path.
On most hardware that means clear-colour compression metadata, valid only for the one value the resource was created with.
Creating without it is legal and costs a slower clear, which is the `did not pass any clear value to resource creation` advisory both test listeners allowlist (`dx12_expected_messages.hh`).

**We pass `nullptr` everywhere today, and that is the right default rather than an oversight.**
sg lets any render pass clear to any colour (`rt.cleared(colour)`), so a stored value that disagrees with the actual clear is a *worse* outcome than none.
D3D12 then raises `CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE` and takes the slow path anyway.

So the shape is an **optional** field a caller opts into when the clear really is fixed.
That is the common case for a depth target cleared to 1.0 every frame, and for a render target with a constant background.
Vulkan has no creation-time equivalent, so it would be a hint one backend honours and the other ignores.
Acceptable for a pure performance hint, and worth stating in [concepts/views.md](../concepts/views.md) if it lands.

Worth doing only with a measurement behind it: a fast clear is a bandwidth win on a full-screen target and noise on a small one.
