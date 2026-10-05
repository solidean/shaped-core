#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh> // sg::hit_shader
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/shader_generator.hh>

/// One material permutation, generated and compiled: the hit group the path tracer traces with, and the parameter layout an
/// instance block is filled from.
///
/// Both come from one `generate_material_shader` call, which is what keeps the layout the CPU fills and the offsets the shader
/// reads from being two independent computations.
struct sv::material_permutation
{
    /// what this was generated from and how — `material_shader_key`, which is also what the cache is keyed on
    cc::hash128 key;

    material_parameter_layout layout;

    /// Which geometry the hit group traces, and so which stand-in replaces it while it compiles.
    ///
    /// A `quadrics` group carries an intersection shader, which is what makes it PROCEDURAL, and that is a property of the
    /// acceleration structure rather than a choice: a procedural BLAS must be traced by a group that has one, and a triangle
    /// BLAS by one that does not.
    geometry_kind kind = geometry_kind::triangles;

    /// Whether this permutation's material ever writes `geometry_opacity` — see `generated_material_shader::can_cut_out`.
    /// Only a triangle group carries the cutout any-hits, so this is false for every quadric one.
    bool can_cut_out = false;

    /// The hit group, a hit shader per ray type of the tracer's ray set, as a cold async node.
    ///
    /// Compiled through `slib::compile_hit_group` against `shaders/tracer_pipeline.sgl`'s pipeline once something starts it,
    /// which `pathtrace_routine` does the first time a trace names this permutation.
    /// Null value while in flight or on a compile error; the routine treats an unfinished permutation the way it treats a
    /// broken shader edit, by substituting the stand-in of its kind.
    /// An error where the cache was created without a context.
    cc::shared_async<cc::vector<sg::hit_shader>> hit_group;

    /// kept for diagnostics: a compile error names a line in this text, and nothing else can reproduce it
    cc::string source;

    /// What a compile error calls this permutation: the material type's name, and whether it is the quadric form.
    cc::string label;

    /// Whether the routine has logged this hit group's compile error, which it does once rather than every frame.
    mutable bool is_failure_reported = false;
};

/// Generates and compiles one hit group per material permutation, deduplicated on `material_shader_key`.
///
/// This is where the two keys pay off.
/// Gold and copper resolve to the same `permutation_key`, so they generate the same source and share this one compile.
/// Only a texture sample — the one thing that changes the generated text — forces a second.
///
/// Compiles go through `sv::acquire_shader_library`, so a generated source resolves its `use`s against the same module
/// directories the tracer's own package mounts.
/// **A generated permutation does not hot-reload when a module it uses is edited.**
/// The key hashes the resolution and the geometry kind rather than the modules' contents — see [docs/TODO.md](../../../docs/TODO.md).
///
/// **Nothing is evicted.** A permutation is a compiled hit group a live pipeline may hold, and the set is bounded by the
/// material types a scene uses rather than by its instance count.
/// Not thread-safe, like the rest of the render path's setup.
class sv::material_shader_cache
{
public:
    /// A cache compiling for `ctx`, which must outlive it; null gives every permutation a hit group that fails.
    [[nodiscard]] static material_shader_cache create(sg::context* ctx);

    /// The permutation for `r` as `kind` spells it, generated on a miss, or the resident one (O(1) on its key).
    ///
    /// **One material, two spellings, one cache.** The kind picks the hit group's stages, and `material_shader_key` folds it
    /// in, so a material used on a mesh and on a quadric batch is two entries here rather than two caches.
    /// `r` must have been resolved against a `geometry_view` of this same kind, or it may name a frequency the chosen
    /// stages cannot read.
    ///
    /// The hit group is handed back cold: nothing compiles until the tracer starts it.
    /// The returned reference is stable across later acquires.
    material_permutation const& acquire(resolved_material const& r, geometry_kind kind = geometry_kind::triangles);

    /// The permutation for `r` spelled for a quadric batch — `acquire(r, geometry_kind::quadrics)`.
    material_permutation const& acquire_quadric(resolved_material const& r);

    /// The neutral permutation a material of `kind` degrades to, generated once per kind per cache.
    ///
    /// It is a real permutation over a type with an EMPTY signature, which is what makes it read no per-instance
    /// parameter block at all — so it can stand in for any material whatever that material's layout was.
    /// It cuts out nothing and samples nothing, so it needs no any-hit either.
    ///
    /// **There is one per geometry kind, and that is not symmetry**: a substitution has to keep the hit group's kind,
    /// since a procedural BLAS traced by a group with no intersection shader reports no hits at all.
    ///
    /// What it buys: one material still compiling, or one that does not compile, degrades to gray shading on its own
    /// geometry instead of making the whole view a no-op.
    material_permutation const& acquire_fallback(geometry_kind kind = geometry_kind::triangles);

    /// The neutral quadric permutation — `acquire_fallback(geometry_kind::quadrics)`.
    material_permutation const& acquire_quadric_fallback();

    /// The permutation for `key`, or null if nothing has acquired it.
    [[nodiscard]] material_permutation const* find(cc::hash128 key) const;

    [[nodiscard]] isize count() const { return _by_key.size(); }

private:
    // A map rather than a vector for the references: a caller holds a permutation while acquiring the next one, and cc::map keeps
    // those valid across every later insert.
    cc::map<cc::hash128, material_permutation> _by_key;
    sg::context* _context = nullptr;
};
