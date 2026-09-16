#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <shaped-viewer/scene/quadric.hh>
#include <shaped-viewer/scene/resident_mesh.hh> // mesh_attribute_binding, which the resident form reuses
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/transform/transform.hh>

/// One quadric batch as resources: the uploaded primitives and the BLAS built over them, placed by a transform and drawn by a
/// material.
///
/// The GPU half of the pair `sv::quadric_set` completes, mirroring `sv::resident_mesh`.
/// It cannot exist without a resource manager, which is what makes it the form that admits a batch produced on the GPU and never
/// held on the CPU at all.
///
/// `bounds` and `primitive_count` are the CPU-side summary, kept for the same reason `resident_mesh` keeps its: with GPU-only
/// data nothing else can answer a camera-framing question, and a placeholder drawn while the real batch is still arriving needs
/// an extent to be drawn at.
struct sv::resident_quadric_set
{
    cc::string name;

    /// the uploaded primitives and the procedural BLAS built from them
    quadric_set_id geometry = quadric_set_id::invalid;

    cc::vector<mesh_attribute_binding> attributes;

    tg::affine_transform3f transform = {};

    material_id material = material_id::invalid;

    /// the extent in the set's own frame — empty when nothing declared one
    cc::optional<tg::aabb3f> bounds;

    isize primitive_count = 0;
};

/// What placing a quadric set produced — the counterpart of `sv::impl::mesh_gpu_slot`, and a cache rather than an identity.
///
/// The set's payload is content-hashed, so placing one against a manager that has never seen it mints the resources the slot
/// would have held anyway.
/// What the slot buys is that a repeat placement is a pointer compare instead of a hash lookup, and that `is_ready` is
/// answerable from the set alone.
struct sv::impl::quadric_set_gpu_slot
{
    /// Identity ONLY, and never dereferenced — a set may outlive the manager it was placed against.
    void const* manager = nullptr;

    /// The resources minted for this set against that manager.
    sv::resident_quadric_set resources;

    /// Whether those resources had reached the GPU, as of that placement.
    bool ready = false;
};

/// A batch of quadric primitives drawn as one thing: one procedural BLAS, one TLAS instance, one material.
///
/// **This is the set a caller builds and holds**, and it is the quadric counterpart of `sv::mesh`.
/// It needs no device to exist, and placing it is what turns its primitives into resources.
///
/// Primitives are added rather than assigned, and `add` is the only way in, because the content hash and the bounds are folded
/// as each one arrives.
/// That is what makes "equal contents give equal hashes" an invariant of the type instead of something a caller has to
/// remember, and it costs O(1) per primitive rather than a pass over the whole batch per frame.
///
/// **The primitives live in the SET's space, and `transform` places that space in the world.**
/// So a set built once can be placed many times, and a non-uniform scale in the placement turns its spheres into ellipsoids —
/// which costs nothing here, because a general quadric is closed under an affine map where a typed sphere would not be.
///
/// One material for the whole batch: a two-material drawing is two sets, which is cheap because the instance count is per set
/// rather than per primitive.
/// Per-primitive variation travels in `attributes` instead, at `per_triangle` — which on a batch means one value per
/// quadric, indexed by `PrimitiveIndex()`.
struct sv::quadric_set
{
    /// human-readable, for debugging and for picking a set out of a scene; not an identity — nothing dedupes on it
    cc::string name;

    /// Arbitrary extra data, looked up by name by the material.
    ///
    /// A batch numbers its primitives and nothing else, so the frequencies it can serve are `per_instance` and `per_triangle`
    /// — one value for the whole placement, or one per primitive indexed by `PrimitiveIndex()`.
    /// Anything finer loses to the coarser rank, exactly as an unusable candidate does on a mesh.
    cc::vector<mesh_attribute> attributes;

    /// placement of the set's own space in the world; may scale or shear, so build it from tg's factories and `tg::compose`
    tg::affine_transform3f transform = {};

    /// how this set is drawn — the definition lives elsewhere and is shared
    material_id material = material_id::invalid;

    /// What placing this set produced — see `impl::quadric_set_gpu_slot`.
    /// Mutable because placing a set READS it: `scene_ref::add_quadrics` takes a `quadric_set const&`.
    mutable impl::quadric_set_gpu_slot cache;

    /// Appends the sphere `s`.
    void add_sphere(tg::sphere3f const& s) { add(quadric_primitive::create_sphere(s)); }

    /// Appends the segment `s` drawn in `style` — one primitive for `flat` and `open` ends, three for `round`.
    ///
    /// The default is an OPEN tube, which is what a mesh's edges want: the joints already carry vertex spheres, so a cap
    /// there would draw geometry nothing can see.
    /// Between `open` and `flat` the primitive's box is identical, so that choice costs nothing but a bit.
    void add_line(tg::segment3f const& s, line_style const& style)
    {
        for (auto const& p : line_primitives(s, style))
            add(p);
    }

    /// The same at `radius`, with the default ends — see `sv::line_style`.
    void add_line(tg::segment3f const& s, float radius) { add_line(s, {.radius = radius}); }

    /// Appends the cone whose base disc is the circle of radius `base_radius` about `base_to_apex.pos0`, tipped at `pos1`.
    /// `capped` draws that base disc; without it the cone is open and shows its own interior.
    void add_cone(tg::segment3f const& base_to_apex, float base_radius, bool capped = true)
    {
        add(quadric_primitive::create_cone(base_to_apex, base_radius, capped));
    }

    /// Appends an arrow from `s.pos0` to `s.pos1` — a capped cylinder for the shaft plus a cone for the head, so two
    /// primitives.
    ///
    /// The tip is exactly `s.pos1`: the head is taken out of the segment rather than added past its end.
    /// This overload takes every length from the arrow's own, which is what makes one arrow a one-liner; the other two fix
    /// the shaft radius instead, which is what a field of arrows that must look alike wants.
    void add_arrow(tg::segment3f const& s) { add_arrow(s, arrow_style::for_length((s.pos1 - s.pos0).length())); }

    /// The same with the head scaled to `shaft_radius` — `arrow_style::for_shaft_radius`.
    void add_arrow(tg::segment3f const& s, float shaft_radius)
    {
        add_arrow(s, arrow_style::for_shaft_radius(shaft_radius));
    }

    /// The same with all three lengths given.
    void add_arrow(tg::segment3f const& s, arrow_style const& style);

    /// Appends a round-capped segment — the cylinder plus a sphere at each end, so three primitives rather than one.
    /// A capsule's surface is not degree 2, which is why it cannot be one.
    /// The same as `add_line` at `line_ends::round`, named because a capsule is a shape a caller asks for by name.
    void add_capsule(tg::segment3f const& s, float radius);

    /// Appends an already-built primitive, for a shape the named factories do not cover.
    void add(quadric_primitive const& p);

    void reserve(isize count) { _primitives.reserve(count); }

    /// Drops every primitive, and with them the hash and the bounds.
    /// Leaves `name`, `attributes`, `transform` and `material` alone, so a per-frame set can be refilled without rebuilding them.
    void clear();

    [[nodiscard]] cc::span<quadric_primitive const> primitives() const { return _primitives; }
    [[nodiscard]] isize primitive_count() const { return _primitives.size(); }
    [[nodiscard]] bool is_empty() const { return _primitives.empty(); }

    /// The content key the resource managers cache by.
    /// Folded as primitives arrive, and order-sensitive because primitive order IS `PrimitiveIndex()`.
    [[nodiscard]] cc::hash128 hash() const { return _hash; }

    /// The extent of every primitive, in the set's own space.
    /// Empty while the set is, which is what a placeholder needs to know it has nothing to stand in for yet.
    [[nodiscard]] cc::optional<tg::aabb3f> const& bounds() const { return _bounds; }

    /// Whether this set's resources had reached the GPU, as of the last time it was placed.
    /// False for a set nobody has placed, which is the honest answer rather than a special case.
    [[nodiscard]] bool is_ready() const { return cache.ready; }

private:
    cc::vector<quadric_primitive> _primitives;

    /// folded per primitive rather than computed over the buffer, so adding one is O(1) and placing one re-hashes nothing
    cc::hash128 _hash;

    cc::optional<tg::aabb3f> _bounds;
};
