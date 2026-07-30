#pragma once

// InlineConstantBuffer<T> — a constant buffer fed as inline (root / push) constants rather than a bound
// buffer: the routine supplies it through pipeline_layout_description::inline_constants +
// cmd.*.set_inline_constants, and keeps the b-register out of the binding group.
//
// [[vk::push_constant]] pins it to the SPIR-V push-constant block. DXC ignores the vk:: attribute on the
// DXIL path, where the same declaration reflects as an ordinary b-register cbuffer that the pipeline layout
// promotes to root constants — so one declaration is correct on both backends.
//
// Usage: InlineConstantBuffer<my_constants> pc : register(b0);  then read pc.field.
#define InlineConstantBuffer [[vk::push_constant]] ConstantBuffer
