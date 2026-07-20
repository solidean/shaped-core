#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/buffer.hh> // typed buffer<T> — the preferred overloads below take it
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/texture_region.hh>

#include <ranges>
#include <type_traits>

namespace sg
{
/// Host→device upload facade for a command list, reached as `cmd.upload`.
///
/// A thin facade over its owning command list: it forwards each op to the list's backend impl.
class command_list_upload_scope
{
    // Typed-buffer overloads — the preferred form. `buffer<T>` alone names the element type, so the span,
    // the value and `offset_in_elements` are all in units of that same `T`; anything else is a compile
    // error at the argument, not a static_assert inside. (`T` is deduced only from the buffer, so the data
    // argument converts to `span<T const>` the usual way — from a vector, a C array, a braced list, …)
    // Drop to the raw_buffer_handle overloads below (via `.raw()`) only for byte-addressed work.
public:
    /// Uploads a span of `T` into `dst` at `offset_in_elements`. See bytes_to_buffer.
    template <class T>
    void data_to_buffer(buffer<T> const& dst, std::type_identity_t<cc::span<T const>> data, isize offset_in_elements = 0)
    {
        data_to_buffer(dst.raw(), data, offset_in_elements);
    }

    /// Uploads a single `T` into `dst` at `offset_in_elements` — the one-element case. See bytes_to_buffer.
    template <class T>
    void pod_to_buffer(buffer<T> const& dst, std::type_identity_t<T> const& value, isize offset_in_elements = 0)
    {
        pod_to_buffer(dst.raw(), value, offset_in_elements);
    }

    // Raw overloads — element type supplied by the call site rather than the buffer.
public:
    /// Uploads `data` into `buffer` starting at `offset_in_bytes`.
    /// The byte-level escape hatch — prefer data_to_buffer / pod_to_buffer, which count in elements.
    /// Buffer must have buffer_usage::copy_dst; offset_in_bytes + data.size() must fit it.
    /// The source bytes are copied immediately: safe to mutate or free them once this returns.
    /// The write is visible to later commands in the same list. Empty span = no-op.
    /// TODO: version with pinned_data that tries to copy it in parallel and blocks on submit?
    void bytes_to_buffer(raw_buffer_handle buffer, cc::span<cc::byte const> data, isize offset_in_bytes = 0);

    /// Uploads a trivially-copyable contiguous range. `offset_in_elements` counts the range's value type.
    /// See bytes_to_buffer for the contract.
    template <std::ranges::contiguous_range RangeT>
    void data_to_buffer(raw_buffer_handle buffer, RangeT const& data, isize offset_in_elements = 0)
    {
        using element_t = std::remove_cvref_t<std::ranges::range_value_t<RangeT>>;
        static_assert(std::is_trivially_copyable_v<element_t>, "upload element type must be trivially copyable");
        bytes_to_buffer(cc::move(buffer), cc::as_bytes(data), offset_in_elements * isize(sizeof(element_t)));
    }

    /// Uploads a single trivially-copyable value — the one-element case of data_to_buffer.
    /// `offset_in_elements` counts `T`, so it addresses the buffer as an array of `T`.
    /// Drop to bytes_to_buffer for an offset that is not a multiple of sizeof(T).
    /// See bytes_to_buffer for the contract.
    template <class T>
    void pod_to_buffer(raw_buffer_handle buffer, T const& value, isize offset_in_elements = 0)
    {
        static_assert(std::is_trivially_copyable_v<T>, "upload value type must be trivially copyable");
        bytes_to_buffer(cc::move(buffer), cc::span<T const>(&value, 1).as_bytes(), offset_in_elements * isize(sizeof(T)));
    }

    /// Uploads tightly-packed `pixels` into one `subresource` of `texture`.
    /// `region` selects a box within the subresource; none fills the **whole subresource**, empty = no-op.
    /// Texture must have texture_usage::copy_dst.
    /// The source bytes are copied immediately (safe to mutate/free once this returns); the write is visible to later commands in the same list.
    /// Preconditions: the subresource exists; `region` is in bounds and block-aligned for a block-compressed format.
    /// `pixels.size()` equals the box's tightly-packed byte size (rows = height-in-blocks, row bytes = width-in-blocks × block-bytes).
    void bytes_to_texture(raw_texture_handle texture,
                          cc::span<cc::byte const> pixels,
                          subresource_index const& subresource = {},
                          cc::optional<texture_region> region = {});

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_upload_scope(command_list_upload_scope const&) = delete;
    command_list_upload_scope(command_list_upload_scope&&) = delete;
    command_list_upload_scope& operator=(command_list_upload_scope const&) = delete;
    command_list_upload_scope& operator=(command_list_upload_scope&&) = delete;

private:
    // Only a command list constructs its own scope; the scope in turn reaches the list's protected
    // backend virtuals (mutual friendship).
    friend class command_list;
    explicit command_list_upload_scope(command_list& cmd) : _cmd(cmd) {}

    command_list& _cmd;
};
} // namespace sg
