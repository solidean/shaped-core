#pragma once

#include <clean-core/common/utility.hh>
#include <shaped-graphics/buffer.hh> // typed buffer<T> — the preferred overloads below take it
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/texture_region.hh>

#include <type_traits>

namespace sg
{
/// Device→host download facade for a command list, reached as `cmd.download`.
///
/// A thin facade over its owning command list: it forwards each op to the list's backend impl.
class command_list_download_scope
{
    // Typed-buffer overloads — the preferred form. `buffer<T>` supplies the element type, so `T` is deduced
    // rather than spelled out and the offset / count are in units of that same `T`.
public:
    /// Reads `count` elements of `T` from `src` starting at `offset_in_elements`. See bytes_from_buffer.
    template <class T>
    [[nodiscard]] data_future<T> data_from_buffer(buffer<T> const& src, isize offset_in_elements, isize count)
    {
        return data_from_buffer<T>(src.raw(), offset_in_elements, count);
    }

    /// Reads the whole of `src` back. See bytes_from_buffer.
    template <class T>
    [[nodiscard]] data_future<T> data_from_buffer(buffer<T> const& src)
    {
        return data_from_buffer<T>(src.raw(), 0, src.element_count());
    }

    // Raw overloads — element type supplied by the call site rather than the buffer.
public:
    /// Reads `size_in_bytes` from `buffer` starting at `offset_in_bytes` back to the host. The buffer
    /// must have been created with buffer_usage::copy_src. Returns a bytes_future that becomes ready
    /// once the submitted list has finished on the GPU and the bytes have been copied to the host. A
    /// zero-size read yields an already-ready, empty future. Precondition: offset_in_bytes +
    /// size_in_bytes <= buffer size.
    [[nodiscard]] bytes_future bytes_from_buffer(raw_buffer_handle buffer, isize offset_in_bytes, isize size_in_bytes);

    /// Downloads `count` elements of a trivially-copyable type; `offset_in_elements` and `count` are in
    /// elements of T. See bytes_from_buffer.
    template <class T>
    [[nodiscard]] data_future<T> data_from_buffer(raw_buffer_handle buffer, isize offset_in_elements, isize count)
    {
        static_assert(std::is_trivially_copyable_v<T>, "download element type must be trivially copyable");
        auto const stride = isize(sizeof(T));
        return data_future<T>(bytes_from_buffer(cc::move(buffer), offset_in_elements * stride, count * stride));
    }

    /// Reads one `subresource` of `texture` back to the host as tightly-packed bytes. `region` selects a box
    /// within the subresource; passing none reads the **whole subresource**, and an empty region returns a
    /// ready, empty future. The texture must have been created with texture_usage::copy_src. Returns a
    /// bytes_future ready once the submitted list has finished on the GPU and the rows have been un-padded
    /// into host memory. The result layout matches bytes_to_texture (rows = height-in-blocks, row bytes =
    /// width-in-blocks × block-bytes). Precondition: a given `region` is in bounds + block-aligned.
    [[nodiscard]] bytes_future bytes_from_texture(raw_texture_handle texture,
                                                  subresource_index const& subresource = {},
                                                  cc::optional<texture_region> region = {});

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
