#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/fwd.hh>

#include <type_traits>

/// Per-element access for a *buffer* array bound to a shader — the payload of `declare_array_buffer_access`.
/// Buffers have no layout, so only the accessed element, stage(s), and access are named.
struct sg::array_buffer_access
{
    int index = 0;                    ///< element index within the bound array
    pipeline_stage_flags stages = {}; ///< stage(s) the shader accesses it in
    access_flags access = {};         ///< how the shader accesses this element
};

/// Per-element access for a *texture* array bound to a shader — the payload of `declare_array_texture_access`.
/// It adds the layout the element must be in.
struct sg::array_texture_access
{
    int index = 0;                                   ///< element index within the bound array
    pipeline_stage_flags stages = {};                ///< stage(s) the shader accesses it in
    access_flags access = {};                        ///< how the shader accesses this element
    texture_layout layout = texture_layout::general; ///< the layout the element must be in
    // A subresource range (which mips / array slices / aspects) is a future addition — see resource/subresource.hh.
};

/// Compute recording facade for a command list, reached as `cmd.compute`: bind a pipeline and resource groups, then dispatch.
/// See libs/graphics/shaped-graphics/docs/concepts/bindings.md for the bind path.
class sg::command_list_compute_scope
{
public:
    /// Makes `pipeline` the active compute pipeline for subsequent bind_group / dispatch calls.
    /// Caches its workgroup size for dispatch_threads.
    void bind_pipeline(compute_pipeline const& pipeline);

    /// Binds `group` to descriptor set `set` of the active pipeline.
    /// The group's layout must match the pipeline's for that set.
    void bind_group(int set, binding_group const& group);

    /// Dispatches `x`*`y`*`z` **workgroups** of the active pipeline.
    void dispatch_groups(int x, int y = 1, int z = 1);

    /// Dispatches enough workgroups to cover `x`*`y`*`z` **threads**, rounding up per axis by the bound pipeline's workgroup size (`ceil(threads / workgroup_size)`).
    /// A pipeline must be bound first.
    void dispatch_threads(int x, int y = 1, int z = 1);

    /// Declares per-element access for a *buffer* array / bindless binding, applied to the **next dispatch only**.
    /// Declare again before each dispatch that needs it.
    /// A scalar binding has its access inferred from the shader and the bound view.
    /// Array element usage cannot be, since a shader may index only some elements, or use them differently.
    /// `binding_name` is the array binding's reflection name, and each `array_buffer_access` names one element and how it is accessed.
    void declare_array_buffer_access(cc::string_view binding_name, cc::span<array_buffer_access const> elements);

    /// Declares per-element access for a *texture* array / bindless binding.
    /// Like the buffer form it applies to the next dispatch only, but each element also names the layout it must be in.
    void declare_array_texture_access(cc::string_view binding_name, cc::span<array_texture_access const> elements);

    /// Writes inline constants into the bound pipeline layout's `inline_constants` block — dx12 root constants / vulkan push constants.
    /// Small per-dispatch data, with no descriptor space.
    /// A pipeline whose layout declares inline_constants must be bound first.
    /// `offset` unset => full replace, and `data.size()` must equal the declared block_size; a value => partial update at that byte offset.
    /// Both `data.size()` and `offset` must be multiples of 4.
    void set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset = {});

    /// POD convenience: bit-copies `value` as the inline-constants payload.
    /// `T` must be trivially copyable with a size that is a multiple of 4 bytes.
    template <class T>
    void set_inline_constants(T const& value, cc::optional<isize> offset = {})
    {
        static_assert(std::is_trivially_copyable_v<T>, "inline-constants payload must be trivially copyable");
        static_assert(sizeof(T) % 4 == 0, "inline-constants payload size must be a multiple of 4 bytes");
        set_inline_constants(cc::as_bytes(cc::span<T const>(&value, 1)), offset);
    }

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_compute_scope(command_list_compute_scope const&) = delete;
    command_list_compute_scope(command_list_compute_scope&&) = delete;
    command_list_compute_scope& operator=(command_list_compute_scope const&) = delete;
    command_list_compute_scope& operator=(command_list_compute_scope&&) = delete;

private:
    // Only a command list constructs its own scope, and the scope reaches the list's protected backend virtuals — mutual friendship.
    friend class command_list;
    explicit command_list_compute_scope(command_list& cmd) : _cmd(cmd) {}

    command_list& _cmd;

    // Workgroup size of the currently-bound pipeline; defaults to 1s, so dispatch_threads == dispatch_groups until a pipeline is bound.
    // Plain scalars, to keep this header dependency-light.
    int _bound_wg_x = 1;
    int _bound_wg_y = 1;
    int _bound_wg_z = 1;
};
