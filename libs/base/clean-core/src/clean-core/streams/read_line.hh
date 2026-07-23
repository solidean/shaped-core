#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/string.hh>

// A small line reader over any readable stream.
// Kept out of the core stream engine on purpose: the engine header is pulled in by every stream user
// (including the allocation-free span adapter), and this helper needs the allocating cc::string.
// It codes strictly against the public window API (ready_bytes / consume / flush), so it works for
// read_stream, read_write_stream and both seekable read variants, and never buffers the whole input.

namespace cc
{
/// Read one line into `out`, excluding the terminator.
/// `out` is cleared first, then filled with the bytes up to (not including) the next '\n';
/// a trailing '\r' from a "\r\n" ending is stripped, so both Unix and Windows line endings work.
/// Returns true when a line was read, false at end-of-data with nothing left to read.
/// The final line of a file that lacks a trailing newline is returned once (true), then the next call returns false.
/// An empty line yields true with an empty `out`.
template <class ReadStream>
    requires requires(ReadStream& s) { s.ready_bytes(); }
[[nodiscard]] cc::result<bool> read_line(ReadStream& in, cc::string& out)
{
    out.clear();
    auto read_any = false;
    while (true)
    {
        auto window = in.ready_bytes();
        if (window.empty())
        {
            CC_RETURN_IF_ERROR(in.flush());
            window = in.ready_bytes();
            if (window.empty())
                return read_any; // genuine end of data
        }
        read_any = true;

        auto const* const base = reinterpret_cast<char const*>(window.data());
        auto newline = isize(-1);
        for (auto i = isize(0); i < window.size(); ++i)
            if (base[i] == '\n')
            {
                newline = i;
                break;
            }

        if (newline >= 0)
        {
            out += cc::string_view(base, newline);
            in.consume(newline + 1); // consume the line and its '\n'
            if (out.size() > 0 && out[out.size() - 1] == '\r')
                out.resize_down_to(out.size() - 1); // strip the '\r' of a "\r\n" ending (possibly split across windows)
            return true;
        }

        // no newline in this window: take all of it and refill
        out += cc::string_view(base, window.size());
        in.consume(window.size());
    }
}
} // namespace cc
