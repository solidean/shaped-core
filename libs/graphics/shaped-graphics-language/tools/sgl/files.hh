#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

/// The two file operations the commands share, with the reason a failure gives.
namespace sgl_tool
{
inline cc::result<cc::string, cc::string> read_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
        return cc::error(adapter.error().to_string());
    auto stream = adapter.value().stream();
    auto const bytes = stream.read_all();
    if (bytes.has_error())
        return cc::error(bytes.error().to_string());
    return cc::string(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());
}

/// Creates or truncates `path`.
inline cc::result<cc::unit, cc::string> write_file(cc::string_view path, cc::string_view content)
{
    auto adapter = cc::file_write_stream_adapter::create(path);
    if (adapter.has_error())
        return cc::error(adapter.error().to_string());
    auto stream = adapter.value().stream();
    if (auto const written = stream.write(cc::as_bytes(content)); written.has_error())
        return cc::error(written.error().to_string());
    // no auto-flush: buffered bytes are lost otherwise
    if (auto const flushed = stream.flush(); flushed.has_error())
        return cc::error(flushed.error().to_string());
    return cc::unit{};
}
} // namespace sgl_tool
