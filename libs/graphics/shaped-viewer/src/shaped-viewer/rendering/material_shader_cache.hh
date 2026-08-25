#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/shader_generator.hh>

/// One material permutation, generated and compiled: the closest-hit the path tracer traces with, and the parameter layout an
/// instance block is filled from.
///
/// Both come from one `generate_material_shader` call, which is what keeps the layout the CPU fills and the offsets the shader
/// reads from being two independent computations.
struct sv::material_permutation
{
    material_parameter_layout layout;

    /// The compiled closest-hit, as a cold async node — nothing compiles until it is driven.
    /// Null value while in flight or on a compile error; `pathtrace_routine` treats an unfinished permutation the way it treats a
    /// broken shader edit, by not tracing with it.
    sg::async_compiled_shader shader;

    /// kept for diagnostics: a compile error names an offset in this text, and nothing else can reproduce it
    cc::string source;
};

/// Generates and compiles one closest-hit per material permutation, deduplicated on `permutation_key`.
///
/// This is where the two keys pay off.
/// Gold and copper resolve to the same `permutation_key`, so they generate the same source and share this one compile.
/// Only a texture sample — the one thing that changes the generated text — forces a second.
///
/// Compiles go through `sv::acquire_shader_library`, so a generated source resolves its `#include`s against the same mounts a
/// hand-authored shader does, and an edit to `material_runtime.hlsli` changes the generated text and therefore the key.
///
/// **Nothing is evicted.** A permutation is a compiled shader a live pipeline may hold, and the set is bounded by the material
/// types a scene uses rather than by its instance count.
/// Not thread-safe, like the rest of the render path's setup.
class sv::material_shader_cache
{
public:
    /// A cache producing shaders in `format`, which must be one the context they are traced on accepts.
    /// The format is fixed per cache rather than per acquire: a permutation is keyed by its source, so two formats of one
    /// permutation would collide on that key.
    [[nodiscard]] static material_shader_cache create(sg::shader_format format);

    [[nodiscard]] sg::shader_format format() const { return _format; }

    /// The permutation for `r`, generated and compiled on a miss, or the resident one (O(1) on `r.permutation_key`).
    /// The returned reference is stable across later acquires.
    material_permutation const& acquire(resolved_material const& r);

    /// The permutation for `key`, or null if nothing has acquired it.
    [[nodiscard]] material_permutation const* find(cc::hash128 key) const;

    [[nodiscard]] isize count() const { return _by_key.size(); }

private:
    // A map rather than a vector for the references: a caller holds a permutation while acquiring the next one, and cc::map keeps
    // those valid across every later insert.
    cc::map<cc::hash128, material_permutation> _by_key;
    sg::shader_format _format = sg::shader_format::dxil;
};
