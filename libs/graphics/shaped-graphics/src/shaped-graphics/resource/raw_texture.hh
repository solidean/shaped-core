#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/types.hh>

#include <atomic>

namespace sg
{
/// Rank of a texture's coordinate grid — how many spatial axes it has.
/// Array-ness, cube-ness and multisampling are orthogonal, carried separately on texture_description rather than as extra dimensions.
enum class texture_dimension : u8
{
    d1, ///< width only
    d2, ///< width + height
    d3, ///< width + height + depth
};

} // namespace sg

/// The immutable shape of a texture: everything a backend needs to allocate the GPU resource.
/// Shape is derived, not duplicated in redundant flags — libs/graphics/shaped-graphics/docs/concepts/textures.md has the reasoning.
///   - `dimension` alone decides which extents are meaningful: d1 -> width, d2 -> +height, d3 -> +depth.
///     The rest stay 1.
///   - `array_layers` is nullopt for a non-array texture and a count, 1 included, for an array.
///     So a plain 2D texture is distinct from a single-slice 2D array, with no separate flag to keep in step.
///   - `is_cube` is orthogonal: a cube array is `is_cube` plus `array_layers = N`, which is `6 * N` faces internally.
struct sg::texture_description
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

    /// Whether the shape contract holds: a concrete format, extents >= 1, mip and sample counts >= 1, and a valid dimension / array / cube / MSAA combination.
    /// The non-asserting counterpart of assert_valid().
    [[nodiscard]] bool is_valid() const;

    /// Asserts the shape contract one invariant at a time — is_valid says what the contract is.
    /// Runs from raw_texture's constructor, and a backend calls it at the top of its create path so the contract is enforced before any fallible GPU work.
    void assert_valid() const;
};

/// A GPU-resident texture of immutable shape, its `texture_description`.
/// Contents change through command lists; format, extents, mips, layers and samples are fixed at creation.
/// This is the *raw*, general resource — a minimal API over the description, held via `raw_texture_handle`.
/// The typed `texture<Traits>` wrapper (texture.hh) adds shape-checked, concept-gated accessors on top.
///
/// Abstract: a backend subclasses it and owns the GPU resource, reading the description below directly.
class sg::raw_texture : public std::enable_shared_from_this<raw_texture>
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

    /// Whether this texture is multisampled.
    [[nodiscard]] bool is_multisampled() const { return _desc.sample_count > 1; }

    // Re-type this raw texture as a strongly-typed `texture<Traits>` wrapper — one accessor per shape typedef, since the shape cannot be inferred.
    // `as_texture_2d` asserts the runtime shape matches (see texture_traits::matches); `try_as_texture_2d` returns nullopt instead.
    // Equivalent to `texture_2d::from_raw(handle)`, reached straight off the handle.
    // Defined in texture.hh, where the wrapper and the shape typedefs are complete.
    [[nodiscard]] auto as_texture_1d() const;                // -> texture_1d
    [[nodiscard]] auto try_as_texture_1d() const;            // -> cc::optional<texture_1d>
    [[nodiscard]] auto as_texture_2d() const;                // -> texture_2d
    [[nodiscard]] auto try_as_texture_2d() const;            // -> cc::optional<texture_2d>
    [[nodiscard]] auto as_texture_3d() const;                // -> texture_3d
    [[nodiscard]] auto try_as_texture_3d() const;            // -> cc::optional<texture_3d>
    [[nodiscard]] auto as_texture_cube() const;              // -> texture_cube
    [[nodiscard]] auto try_as_texture_cube() const;          // -> cc::optional<texture_cube>
    [[nodiscard]] auto as_texture_1d_array() const;          // -> texture_1d_array
    [[nodiscard]] auto try_as_texture_1d_array() const;      // -> cc::optional<texture_1d_array>
    [[nodiscard]] auto as_texture_2d_array() const;          // -> texture_2d_array
    [[nodiscard]] auto try_as_texture_2d_array() const;      // -> cc::optional<texture_2d_array>
    [[nodiscard]] auto as_texture_cube_array() const;        // -> texture_cube_array
    [[nodiscard]] auto try_as_texture_cube_array() const;    // -> cc::optional<texture_cube_array>
    [[nodiscard]] auto as_texture_2d_ms() const;             // -> texture_2d_ms
    [[nodiscard]] auto try_as_texture_2d_ms() const;         // -> cc::optional<texture_2d_ms>
    [[nodiscard]] auto as_texture_2d_array_ms() const;       // -> texture_2d_array_ms
    [[nodiscard]] auto try_as_texture_2d_array_ms() const;   // -> cc::optional<texture_2d_array_ms>
    [[nodiscard]] auto as_texture_cube_ms() const;           // -> texture_cube_ms
    [[nodiscard]] auto try_as_texture_cube_ms() const;       // -> cc::optional<texture_cube_ms>
    [[nodiscard]] auto as_texture_cube_array_ms() const;     // -> texture_cube_array_ms
    [[nodiscard]] auto try_as_texture_cube_array_ms() const; // -> cc::optional<texture_cube_array_ms>

    /// Register a callback to run once this texture's GPU storage is released *and* its owning epoch has retired.
    /// The feedback point for reclaiming externally-owned backing memory.
    /// Do not assume which thread runs it.
    /// Const because registering a finalizer is a lifetime hook.
    void add_finalizer(cc::unique_function<void()> finalizer) const { _finalizers.push_back(cc::move(finalizer)); }

    // Expiry — a texture may be marked expired, its storage reclaimed, while handles to it still exist.
    // Naming an expired texture is invalid.

    /// Whether this texture's storage has been reclaimed.
    /// Once true, it never goes back to false.
    [[nodiscard]] bool is_expired() const { return _expired.load(std::memory_order_acquire); }

    /// The negation of is_expired(): the texture still names live storage.
    [[nodiscard]] bool is_valid() const { return !is_expired(); }

    /// Expire the texture now, releasing its GPU storage — deferred until it is no longer in flight.
    /// Idempotent, and const because expiry is a lifetime operation, not a change to the texture's shape.
    void expire() const
    {
        if (!_expired.exchange(true, std::memory_order_acq_rel))
            on_expired();
    }

protected:
    explicit raw_texture(texture_description const& desc);

    /// Backend hook run once from `expire()`, after the texture is marked expired: release the GPU storage.
    /// Backends defer that until the owning epoch retires, and the default has nothing to release.
    virtual void on_expired() const {}

    texture_description _desc;
    mutable cc::vector<cc::unique_function<void()>> _finalizers; // mutable: add_finalizer is const (a lifetime hook)
    mutable std::atomic<bool> _expired = {false};                // mutable: expire() is a const lifetime hook
};
