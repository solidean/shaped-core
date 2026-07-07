#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/fwd.hh>

#include <ranges>
#include <type_traits>

namespace sg
{
/// Host→device upload facade for a command list, reached as `cmd.upload`.
///
/// A thin facade over its owning command list: it forwards each op to the list's backend impl.
class command_list_upload_scope
{
public:
    /// Uploads `data` into `buffer` starting at `offset_in_bytes`. The buffer must have been created
    /// with buffer_usage::copy_dst. The source bytes are copied immediately, so it is safe to mutate
    /// or free them once this returns. The write is visible to later commands in the same list. An
    /// empty span is a no-op. Precondition: offset_in_bytes + data.size() <= buffer size.
    /// TODO: version with pinned_data that tries to copy it in parallel and blocks on submit?
    void bytes_to_buffer(buffer_handle buffer, cc::span<cc::byte const> data, cc::isize offset_in_bytes = 0);

    /// Uploads a trivially-copyable contiguous range as raw bytes. `offset_in_elements` is in elements
    /// of the range's value type. See bytes_to_buffer.
    template <std::ranges::contiguous_range RangeT>
    void data_to_buffer(buffer_handle buffer, RangeT const& data, cc::isize offset_in_elements = 0)
    {
        using element_t = std::remove_cvref_t<std::ranges::range_value_t<RangeT>>;
        static_assert(std::is_trivially_copyable_v<element_t>, "upload element type must be trivially copyable");
        bytes_to_buffer(cc::move(buffer), cc::as_bytes(data), offset_in_elements * cc::isize(sizeof(element_t)));
    }

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
