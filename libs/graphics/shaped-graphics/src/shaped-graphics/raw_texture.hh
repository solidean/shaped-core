#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/types.hh>

#include <atomic>
#include <memory>

namespace sg
{
/// Rank of a texture's coordinate grid — how many spatial axes it has. Array-ness, cube-ness and
/// multisampling are orthogonal (carried separately on texture_description), not extra dimensions.
enum class texture_dimension : u8
{
    d1, ///< width only
    d2, ///< width + height
    d3, ///< width + height + depth
};

/// The immutable shape of a texture: everything a backend needs to allocate the GPU resource. Shape is
/// derived, not duplicated in redundant flags:
///   - `dimension` alone decides which extents are meaningful (d1 → width; d2 → +height; d3 → +depth);
///     non-meaningful extents stay 1.
///   - `array_layers` is `nullopt` for a non-array texture and a count (incl. 1) for an array — so a
///     plain 2D texture is distinct from a single-slice 2D array with no separate flag.
///   - `sample_count > 1` means multisampled.
///   - `is_cube` is orthogonal: a cube is `is_cube=true, array_layers=nullopt`; a cube array is
///     `is_cube=true, array_layers=N` (a face count of `6 * N` internally).
struct texture_description
{
    pixel_format format = pixel_format::undefined;
    texture_dimension dimension = texture_dimension::d2;

    int width = 1;
    int height = 1; ///< meaningful for d2 / d3
    int depth = 1;  ///< meaningful for d3

    int mip_levels = 1;
    cc::optional<int> array_layers = {}; ///< nullopt = not an array; a value (incl. 1) = array slice count
    int sample_count = 1;                ///< > 1 = multisampled
    bool is_cube = false;

    texture_usage usage = texture_usage::none;
};

/// A GPU-resident texture with immutable shape (its texture_description). Contents change through
/// command lists; the shape (format, extents, mips, layers, samples) is fixed at creation. Held via
/// raw_texture_handle. This is the *raw*, general resource — a minimal API over the description; the
/// typed `texture<Traits>` wrapper (texture.hh) adds shape-checked, concept-gated accessors on top.
///
/// Abstract: a backend subclasses it and owns the GPU resource. The description lives here as a
/// protected member that backends read directly.
class raw_texture : public std::enable_shared_from_this<raw_texture>
{
public:
    virtual ~raw_texture();

    /// The full shape this texture was created with.
    [[nodiscard]] texture_description const& description() const { return _desc; }

    [[nodiscard]] pixel_format format() const { return _desc.format; }
    [[nodiscard]] texture_dimension dimension() const { return _desc.dimension; }
    [[nodiscard]] int width() const { return _desc.width; }
    [[nodiscard]] int height() const { return _desc.height; }
    [[nodiscard]] int depth() const { return _desc.depth; }
    [[nodiscard]] int mip_levels() const { return _desc.mip_levels; }
    [[nodiscard]] int sample_count() const { return _desc.sample_count; }
    [[nodiscard]] texture_usage usage() const { return _desc.usage; }

    // Derived shape queries (see texture_description for the encoding).

    /// Whether this is an array texture (any slice count, including 1).
    [[nodiscard]] bool is_array() const { return _desc.array_layers.has_value(); }

    /// Array slice count — the stored count for an array, else 1 for a non-array texture.
    [[nodiscard]] int array_layers() const { return _desc.array_layers.value_or(1); }

    /// Whether the slices are interpreted as cube faces.
    [[nodiscard]] bool is_cube() const { return _desc.is_cube; }

    /// Whether this texture is multisampled (`sample_count > 1`).
    [[nodiscard]] bool is_multisampled() const { return _desc.sample_count > 1; }

    /// Registers a callback to run once this texture's GPU storage is released *and* no longer in flight
    /// (its owning epoch has retired). The feedback point for reclaiming externally-owned backing memory.
    /// Do not assume which thread runs it. Const: registering a finalizer is a lifetime hook.
    void add_finalizer(cc::unique_function<void()> finalizer) const { _finalizers.push_back(cc::move(finalizer)); }

    // Expiry — a texture may be marked expired (its storage reclaimed) while handles to it still exist;
    // naming an expired texture is invalid. A transient texture is auto-expired when its epoch advances;
    // a persistent one can be expired explicitly to free its storage early without dropping every handle.

    /// Whether this texture's storage has been reclaimed. Once true, it never goes back to false.
    [[nodiscard]] bool is_expired() const { return _expired.load(std::memory_order_acquire); }

    /// The negation of is_expired(): the texture still names live storage.
    [[nodiscard]] bool is_valid() const { return !is_expired(); }

    /// Expire the texture now, releasing its GPU storage (deferred until no longer in flight). Idempotent.
    /// Const: expiry is a lifetime operation, not a change to the texture's shape.
    void expire() const
    {
        if (!_expired.exchange(true, std::memory_order_acq_rel))
            on_expired();
    }

protected:
    explicit raw_texture(texture_description const& desc);

    /// Backend hook run once, from expire(), after the texture is marked expired: release the GPU storage
    /// (backends defer it until the owning epoch retires). Default: nothing to release.
    virtual void on_expired() const {}

    texture_description _desc;
    mutable cc::vector<cc::unique_function<void()>> _finalizers; // mutable: add_finalizer is const (a lifetime hook)
    mutable std::atomic<bool> _expired{false};                   // mutable: expire() is a const lifetime hook
};
} // namespace sg
