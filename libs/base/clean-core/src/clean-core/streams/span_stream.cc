#include <clean-core/common/assert.hh>
#include <clean-core/streams/span_stream.hh>

namespace cc::impl
{
cc::result<cc::i64> span_adapter_flush(cc::byte*& curr,
                                       cc::byte*& end,
                                       cc::byte*& write_end,
                                       void* ctx,
                                       cc::i64 offset,
                                       seek_dir dir,
                                       cc::byte* /*first_write*/)
{
    auto const& self = *static_cast<span_adapter_state const*>(ctx);
    cc::byte* const base = self.base;
    cc::isize const size = self.size;
    cc::i64 const pos = cc::i64(curr - base);

    auto reposition = [&](cc::i64 target) -> cc::result<cc::i64>
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
