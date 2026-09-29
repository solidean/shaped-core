#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/binding/shader_stage.hh>
#include <shaped-graphics/fwd.hh>

/// What a shader's code actually does to each binding it could reach, which barrier inference follows at dispatch.
/// See libs/graphics/shaped-graphics/docs/concepts/barriers.md, "Access follows the footprint".

/// Where a footprint came from, which is how far it can be trusted to narrow.
enum class sg::footprint_source : sg::u8
{
    /// Nothing is known: barrier inference falls back to each bound view's class, as if every writable view were written.
    none,

    /// A compiler's reflection: a binding it omits is untouched, and one it keeps is used as its declaration allows.
    reflected,

    /// SGL's own analysis of the code the entry point runs, after inlining.
    exact,
};

/// How one shader's code touches one binding, named as the pipeline layout names it.
///
/// A binding the footprint does not list is untouched, and costs no barrier at all.
/// `access` is spelled in the tracker's own vocabulary: a read through a writable view is `storage_read`, not
/// `shader_read`, because the API wants a different barrier access for it.
struct sg::slot_footprint
{
    cc::string name;
    access_flags access;

    /// An array binding only: some index into it is not a compile-time constant, so which elements it reaches is the
    /// caller's to declare.
    bool dynamic_index = false;

    [[nodiscard]] bool operator==(slot_footprint const&) const = default;
};

/// A compiled shader's footprint: every binding its code touches, and how.
struct sg::shader_footprint
{
    footprint_source source = footprint_source::none;
    cc::vector<slot_footprint> slots;

    [[nodiscard]] bool is_known() const { return source != footprint_source::none; }

    /// `name`'s entry, or null where the code does not touch it.
    [[nodiscard]] slot_footprint const* find(cc::string_view name) const;

    [[nodiscard]] bool operator==(shader_footprint const& rhs) const;
};

/// How a pipeline's code touches one binding slot: the union over every stage that touches it.
struct sg::impl::slot_use
{
    access_flags access;
    pipeline_stage_flags stages;
    bool dynamic_index = false;

    [[nodiscard]] bool is_touched() const { return !access.is_empty(); }
};

/// A pipeline's footprint, resolved against its layout: per group, per position in the group layout's `bindings()`.
///
/// Built once at pipeline creation; a dispatch reads it for every bound view.
/// Unknown when any stage's shader has no footprint — one stage that could write anything makes the whole
/// pipeline's footprint worthless for narrowing.
class sg::impl::pipeline_footprint
{
public:
    /// One stage's shader and the pipeline stages its accesses happen in.
    struct stage_input
    {
        shader_footprint const* footprint = nullptr;
        pipeline_stage_flags stages;
    };

    /// Unknown: every dispatch falls back to the bound views' classes.
    pipeline_footprint() = default;

    [[nodiscard]] static pipeline_footprint resolve(pipeline_layout const& layout, cc::span<stage_input const> stages);

    [[nodiscard]] bool is_known() const { return _known; }

    /// The layout this footprint was resolved against, whether or not it is known; null for a default-constructed one.
    /// Compared by address only, to tell whether two pipelines share one layout.
    [[nodiscard]] pipeline_layout const* layout() const { return _layout; }

    /// Slot `binding` of group `group`, which must be known; an untouched slot reads as empty.
    [[nodiscard]] slot_use use_of(int group, isize binding) const;

    /// The same, looked up by binding name, for the array declarations that address a binding that way.
    [[nodiscard]] slot_use use_of(int group, cc::string_view name) const;

private:
    struct group_uses
    {
        cc::vector<slot_use> slots;
        cc::vector<cc::string> names;
    };

    bool _known = false;
    pipeline_layout const* _layout = nullptr;
    cc::vector<group_uses> _groups;
};

namespace sg
{
/// The footprint a compiler's reflection implies: every binding it reports is touched as its declaration allows,
/// and one it omits is untouched.
/// Sound only where the compiler reports the bindings the compiled code keeps, not every one the source declares.
[[nodiscard]] shader_footprint reflected_footprint(cc::span<binding const> bindings);
} // namespace sg

namespace sg::impl
{
/// The pipeline stages a shader of `stage` runs its accesses in.
[[nodiscard]] pipeline_stage_flags stages_of(shader_stage stage);

/// Logs `message` as an error, once per pipeline, binding and message.
///
/// For a declaration that disagrees with what a pipeline's code does.
/// A hot-reloaded shader can cause one, so it degrades to a conservative barrier rather than asserting, and a frame loop
/// that meets it every frame logs it once.
void log_footprint_mismatch_once(void const* pipeline, cc::string_view binding, cc::string_view message);
} // namespace sg::impl
