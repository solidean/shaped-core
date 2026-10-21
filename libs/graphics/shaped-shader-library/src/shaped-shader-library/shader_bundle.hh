#pragma once

#include <clean-core/container/span.hh>
#include <shaped-shader-library/fwd.hh>

/// Precompiled shaders an executable carries: the bytecode a recording run of the same program compiled, keyed by what it was compiled from.
///
/// A shader_library serves a request the bundle holds without compiling anything, and compiles the rest as it always does.
/// So a bundle is a shortcut and never a requirement: one recorded from another commit simply misses.
/// `SC_SHADER_RECORD=<file>` makes a run write every shader it compiles, which is how a bundle is made;
/// libs/graphics/shaped-shader-library/docs/frozen-shaders.md has the whole flow.

namespace slib
{
/// Adds `bytes`, a bundle a recording run wrote, to what every shader_library serves without compiling.
/// The bytes must outlive the process, which embedded data does: the code `sc_embed_shader_bundle` generates calls this before main.
/// A bundle that does not parse to its end is logged once, and serves the records before the damage.
/// A run that records serves from no bundle, so what it writes is what this build compiles.
void register_shader_bundle(cc::span<byte const> bytes);
} // namespace slib
