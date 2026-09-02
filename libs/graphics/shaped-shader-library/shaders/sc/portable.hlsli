#pragma once

// The portable-HLSL prelude: one source that compiles correctly for every backend slib can target.
// Mounted at `sc` by every shader_library, so `#include "sc/portable.hlsli"` needs no wiring.
// libs/graphics/shaped-shader-library/docs/portable-hlsl.md is the design behind it.
//
// The reason this exists rather than a style guide: DXC reports an unrecognised [[vk::...]] attribute as
// -Wignored-attributes, and ssc compiles with -WX, so an annotation that is not forked on __spirv__ is a
// hard error on DXIL. The fork lives here and in no shader.
//
//   #include "sc/portable.hlsli"
//
//   SC_BINDING(0) Texture2D<float4> albedo;
//   SC_BINDING(0) SamplerState linear_sampler;
//
//   SC_INLINE_CONSTANTS(frame_constants, frame);

#define SC_CAT_(a, b) a##b
#define SC_CAT(a, b) SC_CAT_(a, b)

#ifdef __spirv__
#define SC_ANNOTATE_BINDING(group, index) [[vk::binding(index, group)]]
#define SC_ANNOTATE_INLINE_CONSTANTS [[vk::push_constant]]
#define SC_VERTEX_INPUT(location) [[vk::location(location)]]
#else
#define SC_ANNOTATE_BINDING(group, index)
#define SC_ANNOTATE_INLINE_CONSTANTS
#define SC_VERTEX_INPUT(location)
#endif

// One resource in `group`, indexed by declaration order.
// The index is the number that is easy to get wrong and impossible to check by eye, so __COUNTER__ supplies it; the
// group is one small number that reads where the resource is declared.
//
// Every binding also declares a marker constant named for the slot it took, which is what makes two bindings at one
// slot a redefinition error rather than a collision nobody sees until a pipeline is built.
#define SC_BINDING(group) SC_BINDING_AT(group, __COUNTER__)

// A resource at a group and index both written out, for a slot something outside the shader depends on — a bindless
// table, or a layout built by hand.
#define SC_BINDING_AT(group, index) SC_BINDING_I(group, index)

#define SC_BINDING_I(group, index)                                                           \
    static const uint SC_CAT(SC_CAT(SC_CAT(sc_slot_taken_, group), _), index) = uint(group); \
    SC_ANNOTATE_BINDING(group, index)

// A constant buffer fed as inline constants — pipeline_layout_description::inline_constants plus
// cmd.*.set_inline_constants — rather than bound in a group.
// SPIR-V push constants, DXIL root constants, and on a backend with neither it becomes an ordinary uniform buffer
// the backend feeds, which is why this is the macro to use rather than a cbuffer you promote yourself.
#define SC_INLINE_CONSTANTS(type, name) SC_ANNOTATE_INLINE_CONSTANTS ConstantBuffer<type> name

// SC_VERTEX_INPUT(location) annotates one vertex attribute, numbered in the order sg's vertex layout lists them.
// sg identifies an attribute by its HLSL semantic and SPIR-V has none, so the vulkan backend falls back to position.
// A mismatch is silent: the pipeline builds and the geometry is wrong.
