#pragma once

#include <clean-core/common/utility.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

#include <type_traits>

namespace sg
{
/// Device→host download facade for a command list, reached as `cmd.download`.
///
/// A thin facade over its owning command list: it forwards each op to the list's backend impl.
class command_list_download_scope
{
public:
    /// Reads `size_in_bytes` from `buffer` starting at `offset_in_bytes` back to the host. The buffer
    /// must have been created with buffer_usage::copy_src. Returns a bytes_future that becomes ready
    /// once the submitted list has finished on the GPU and the bytes have been copied to the host. A
    /// zero-size read yields an already-ready, empty future. Precondition: offset_in_bytes +
    /// size_in_bytes <= buffer size.
    [[nodiscard]] bytes_future bytes_from_buffer(buffer_handle buffer, cc::isize offset_in_bytes, cc::isize size_in_bytes);

    /// Downloads `count` elements of a trivially-copyable type. See bytes_from_buffer.
    template <class T>
    [[nodiscard]] data_future<T> data_from_buffer(buffer_handle buffer, cc::isize offset_in_bytes, cc::isize count)
    {
        static_assert(std::is_trivially_copyable_v<T>, "download element type must be trivially copyable");
        return data_future<T>(bytes_from_buffer(cc::move(buffer), offset_in_bytes, count * cc::isize(sizeof(T))));
    }

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_download_scope(command_list_download_scope const&) = delete;
    command_list_download_scope(command_list_download_scope&&) = delete;
    command_list_download_scope& operator=(command_list_download_scope const&) = delete;
    command_list_download_scope& operator=(command_list_download_scope&&) = delete;

private:
    // Only a command list constructs its own scope; the scope in turn reaches the list's protected
    // backend virtuals (mutual friendship).
    friend class command_list;
    explicit command_list_download_scope(command_list& cmd) : _cmd(cmd) {}

    command_list& _cmd;
};
} // namespace sg
