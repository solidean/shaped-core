#include "source_manager.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/streams/file_stream.hh>

namespace scl
{
source_buffer const& source_manager::add_from_text(cc::string text, cc::string_view path)
{
    auto const id = u32(_buffers.size());
    _buffers.push_back(cc::make_unique<source_buffer>(source_buffer::from_text(cc::move(text), path, id)));
    return *_buffers.back().get();
}

cc::result<source_buffer const*> source_manager::add_from_file(cc::string_view path)
{
    // The adapter owns a 4 KiB inline buffer the stream reads through, so it must outlive the stream.
    auto adapter = cc::file_read_stream_adapter::open(path);
    CC_RETURN_IF_ERROR(adapter);

    auto stream = adapter.value().stream();
    auto size = stream.size();
    CC_RETURN_IF_ERROR(size);

    auto bytes = cc::vector<cc::byte>::create_defaulted(size.value());
    CC_RETURN_IF_ERROR(stream.read_full(bytes));

    auto text = cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size()));
    return &add_from_text(cc::move(text), path);
}

source_buffer const& source_manager::buffer(u32 file_id) const
{
    CC_ASSERT(file_id < _buffers.size(), "unknown file_id");
    return *_buffers[file_id].get();
}

cc::string_view source_manager::span_text(source_span s) const
{
    return buffer(s.file_id).span_text(s);
}

resolved_location source_manager::resolve(source_span s) const
{
    auto const& b = buffer(s.file_id);
    auto const lc = b.line_col_at(s.byte_begin);
    return {.path = b.path(), .line = lc.line, .column = lc.column};
}
} // namespace scl
