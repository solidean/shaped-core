#pragma once

#include "fwd.hh"

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

/// LSP's framing on a byte stream: `Content-Length: <n>\r\n`, any other header lines, a blank line, then n bytes of JSON.
///
/// Bytes arrive split anywhere — inside a header, inside a body, several messages at once — so the reader keeps what it
/// has not consumed and hands out whole bodies only.
class lsp::frame_reader
{
public:
    /// Appends `bytes`, and returns every body now complete, in order.
    [[nodiscard]] cc::vector<cc::string> feed(cc::span<cc::byte const> bytes);

    /// True once a header could not be read, or named a header block over 8 KiB or a body over 64 MiB.
    /// Everything after it is dropped, since the stream has lost its framing; bodies complete before it are returned.
    [[nodiscard]] bool is_broken() const { return _is_broken; }

    /// Bytes held that do not yet make a message.
    [[nodiscard]] isize pending_size() const { return _buffer.size(); }

private:
    cc::string _buffer;
    bool _is_broken = false;
};

namespace lsp
{
/// One body as it goes out: the header, then `body`.
[[nodiscard]] cc::string frame(cc::string_view body);
} // namespace lsp
