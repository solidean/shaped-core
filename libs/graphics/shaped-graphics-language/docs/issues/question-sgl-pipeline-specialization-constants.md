---
created: 2026-11-22
tags: [porting-feedback]
---

# Should an @option be specialized when the pipeline is created, not when the shader is compiled

An `@option` is folded at compile time, so a shader with five run-time-selected bool switches became 32 precompiled modules in a port, and its float knobs moved into a uniform.
Every target but dx12 has a form the driver specializes when it creates the pipeline:

- WGSL `override`, given as `constants` of the pipeline's stage.
- vulkan's specialization constants, `OpSpecConstant` and `VkSpecializationInfo`.
- metal's function constants, `[[function_constant(i)]]` and `MTLFunctionConstantValues`.
- dx12 has none: a variant is a compile.

What to decide before any of it is built:

- Whether a specializable option is a kind of its own, `@specialize`, or a way to compile every `@option`.
- What dx12 does: a compile per combination behind the same API, which is what a bake does today.
- How the host names a value, through `sgl describe` and sg's pipeline description, and what the shader cache keys on.
- What SGL itself may still fold: a branch on a specialized constant is no compile-time `const if`.
