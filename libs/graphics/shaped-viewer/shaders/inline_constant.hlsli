#pragma once

// InlineConstantBuffer<T> — a constant buffer fed as inline (root / push) constants rather than a bound
// buffer: the routine supplies it through pipeline_layout_description::inline_constants +
// cmd.*.set_inline_constants, and keeps the b-register out of the binding group.
//
// [[vk::push_constant]] pins it to the SPIR-V push-constant block, and must not reach the DXIL target:
// DXC reports an unrecognised vk:: attribute as -Wignored-attributes, which ssc compiles as an error.
// On DXIL the same declaration is an ordinary b-register cbuffer that the pipeline layout promotes to root
// constants, so the fork is what makes one declaration correct on both backends.
//
// Usage: InlineConstantBuffer<my_constants> pc : register(b0);  then read pc.field.
#ifdef __spirv__
#define InlineConstantBuffer [[vk::push_constant]] ConstantBuffer
#else
#define InlineConstantBuffer ConstantBuffer
#endif
