#include <clean-core/common/assert.hh>
#include <clean-core/streams/span_stream.hh>

namespace cc::impl
{
cc::result<i64>
span_adapter_flush(byte*& curr, byte*& end, byte*& write_end, void* ctx, i64 offset, seek_dir dir, byte* /*first_write*/)
{
    auto const& self = *static_cast<span_adapter_state const*>(ctx);
    byte* const base = self.base;
    isize const size = self.size;
    i64 const pos = i64(curr - base);

    auto reposition = [&](i64 target) -> cc::result<i64>
    {
        if (target < 0 || target > size)
            return cc::error("span stream: seek out of range");
        curr = base + target;
        end = base + size;       // read boundary: the whole span is valid data
        write_end = base + size; // write capacity: the whole span (bounded). For non-rw spans this aliases end.
        return target;
    };

    switch (dir)
    {
    case seek_dir::relative:
        return reposition(pos + offset); // (relative, 0) is the plain flush: curr unchanged, end = base + size
    case seek_dir::begin:
        return reposition(offset);
    case seek_dir::end:
        return reposition(size + offset);
    case seek_dir::dry_relative:
        return pos + offset; // pure query: no clamping, no mutation
    case seek_dir::dry_begin:
        return offset;
    case seek_dir::dry_end:
        return size + offset;
    }
    CC_UNREACHABLE("invalid seek_dir");
}
} // namespace cc::impl
