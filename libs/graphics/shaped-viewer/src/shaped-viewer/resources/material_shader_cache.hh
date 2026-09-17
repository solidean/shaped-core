#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/shader_generator.hh>
#include <shaped-viewer/resources/bindless_tables.hh>

/// One material permutation, generated and compiled: the closest-hit the path tracer traces with, and the parameter layout an
/// instance block is filled from.
///
/// Both come from one `generate_material_shader` call, which is what keeps the layout the CPU fills and the offsets the shader
/// reads from being two independent computations.
struct sv::material_permutation
{
    /// what this was generated from and how — `material_shader_key`, which is also what the cache is keyed on
    cc::hash128 key;

    material_parameter_layout layout;

    /// The sampler states the source declares, in declaration order — `samplers[i]` is `sv_sampler_i`.
    /// A pipeline built over this permutation has to bake them in by name; the generated text carries only the register.
    cc::vector<sg::sampler> samplers;

    /// The compiled closest-hit, as a cold async node — nothing compiles until it is driven.
    /// Null value while in flight or on a compile error; `pathtrace_routine` treats an unfinished permutation the way it treats a
    /// broken shader edit, by not tracing with it.
    sg::async_compiled_shader shader;

    /// The compiled any-hit, which is the cutout test — null unless `can_cut_out`.
    ///
    /// Attaching one to a hit group makes every intersection on it run a shader and costs the hardware its opaque fast path,
    /// so a material that cannot cut out deliberately gets none rather than getting one that always accepts.
    sg::async_compiled_shader any_hit;

    /// The same test compiled against `ShadowPayload`, for the permutation's shadow hit record — null unless `can_cut_out`.
    ///
    /// A second entry point rather than a second hit group carrying the first: an any-hit is invoked with whatever payload
    /// its caller passed and declares exactly one type, so the raygen's ray and a shadow ray cannot share one.
    sg::async_compiled_shader shadow_any_hit;

    /// The compiled intersection shader — null for a triangle permutation, set for a quadric one.
    ///
    /// Its presence is what makes the hit group PROCEDURAL, and that is a property of the acceleration structure rather than a
    /// choice: a procedural BLAS must be traced by a group that has one, and a triangle BLAS by one that does not.
    /// So it is carried per permutation, and `pathtrace_routine` puts it on both of the permutation's records — the shadow one
    /// too, since a shadow ray traverses the same BLAS.
    sg::async_compiled_shader intersection;

    /// Whether this permutation's material ever writes `geometry_opacity` — see `generated_material_shader::can_cut_out`.
    bool can_cut_out = false;

    /// kept for diagnostics: a compile error names an offset in this text, and nothing else can reproduce it
    cc::string source;
};

/// Generates and compiles one closest-hit per material permutation, deduplicated on `material_shader_key`.
///
/// This is where the two keys pay off.
/// Gold and copper resolve to the same `permutation_key`, so they generate the same source and share this one compile.
/// Only a texture sample — the one thing that changes the generated text — forces a second.
///
/// The generation options are fixed per cache and fold into that key, so a cache built over different bindless budgets cannot
/// collide with another's entry for the same resolution.
///
/// Compiles go through `sv::acquire_shader_library`, so a generated source resolves its `#include`s against the same mounts a
/// hand-authored shader does.
/// **A generated permutation does not hot-reload when an include is edited.**
/// The generated text carries a literal `#include` line whose bytes never change when the file does, and the key hashes the
/// resolution and the options rather than the include's contents — see [docs/TODO.md](../../../docs/TODO.md).
///
/// **Nothing is evicted.** A permutation is a compiled shader a live pipeline may hold, and the set is bounded by the material
/// types a scene uses rather than by its instance count.
/// Not thread-safe, like the rest of the render path's setup.
class sv::material_shader_cache
{
public:
    /// The closest-hit entry point every generated permutation defines, in `shaders/pt_material_hit.hlsli`.
    static constexpr cc::string_view hit_entry_point = "PtClosestHit";

    /// The any-hit entry point the same epilogue defines — the cutout test, compiled only where a material can cut out.
    static constexpr cc::string_view any_hit_entry_point = "PtAnyHit";

    /// The same test against `ShadowPayload`, for the shadow hit record.
    static constexpr cc::string_view shadow_any_hit_entry_point = "PtShadowAnyHit";

    /// The epilogue that defines it — what `gpu_resource_manager` hands `create` as `epilogue_include`.
    static constexpr cc::string_view hit_epilogue_include = "pt_material_hit.hlsli";

    // The quadric spelling of the same material: a different runtime and a different epilogue, which `material_shader_key`
    // already folds in — so a quadric permutation and a triangle one of the same material are two entries in this one cache
    // rather than two caches.
    static constexpr cc::string_view quadric_runtime_include = "quadric_runtime.hlsli";
    static constexpr cc::string_view quadric_hit_epilogue_include = "pt_quadric_hit.hlsli";
    static constexpr cc::string_view quadric_hit_entry_point = "PtQuadricClosestHit";
    static constexpr cc::string_view quadric_intersection_entry_point = "QuadricIntersection";

    /// A cache producing shaders in `format`, which must be one the context they are traced on accepts, generated under `opts`.
    /// The format is fixed per cache rather than per acquire: it is not part of the key, so two formats of one permutation
    /// would collide on it.
    /// `opts` IS part of the key, and is copied — a `material_shader_options` borrows its strings and its budgets, and nothing
    /// says the caller's outlive the cache.
    [[nodiscard]] static material_shader_cache create(sg::shader_format format, material_shader_options const& opts = {});

    /// The options every generation here runs under, as `material_shader_key` and `generate_material_shader` take them.
    /// They borrow from the cache, so they are only valid while it is.
    [[nodiscard]] material_shader_options generation_options() const;

    [[nodiscard]] sg::shader_format format() const { return _format; }

    /// The permutation for `r` as `kind` spells it, generated and compiled on a miss, or the resident one (O(1) on its key).
    ///
    /// **One material, two spellings, one cache.** The kind picks the runtime and epilogue includes and the entry points;
    /// `material_shader_key` folds those in, so a material used on a mesh and on a quadric batch is two entries here
    /// rather than two caches, and everything else about the two is the same code.
    /// `r` must have been resolved against a `geometry_view` of this same kind, or it may name a frequency the chosen
    /// runtime cannot read.
    ///
    /// A `quadrics` permutation carries an `intersection` shader, which is what makes its hit group procedural; a
    /// `triangles` one carries the cutout any-hits where its material can cut out.
    /// The returned reference is stable across later acquires.
    material_permutation const& acquire(resolved_material const& r, geometry_kind kind = geometry_kind::triangles);

    /// The permutation for `r` spelled against the quadric runtime and epilogue — `acquire(r, geometry_kind::quadrics)`.
    material_permutation const& acquire_quadric(resolved_material const& r);

    /// The neutral permutation a material of `kind` degrades to, generated and compiled once per kind per cache.
    ///
    /// It is a real permutation over a type with an EMPTY signature, which is what makes it read no per-instance
    /// parameter block at all — so it can stand in for any material whatever that material's layout was.
    /// It cuts out nothing and samples nothing, so it needs no any-hit and no sampler either.
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

    /// The generation options a permutation of `kind` is generated under: this cache's, with that kind's includes.
    [[nodiscard]] material_shader_options generation_options(geometry_kind kind) const;

    [[nodiscard]] isize count() const { return _by_key.size(); }

private:
    // A map rather than a vector for the references: a caller holds a permutation while acquiring the next one, and cc::map keeps
    // those valid across every later insert.
    cc::map<cc::hash128, material_permutation> _by_key;
    sg::shader_format _format = sg::shader_format::dxil;

    // Owned copies of what `create` was handed, since `generation_options` hands out views onto them.
    cc::string _entry_point;
    cc::string _runtime_include;
    cc::string _epilogue_include;
    bindless_config _bindless;
};
