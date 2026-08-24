#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::max
#include <clean-core/streams/growing_stream.hh>

using namespace cc::primitive_defines;

namespace
{
// Free room a flush leaves behind, so the window is never empty and a byte-at-a-time writer still amortizes.
// reserve_back grows exponentially past it, so this is a floor rather than the growth step.
constexpr isize k_min_window = 256;
} // namespace

namespace cc::impl
{
template <class Container>
cc::result<i64> growing_adapter_flush(byte*& curr,
                                      byte*& end,
                                      byte*& write_end,
                                      void* ctx,
                                      i64 offset,
                                      seek_dir dir,
                                      byte* /*first_write*/)
{
    auto& data = *static_cast<Container*>(ctx);
    auto* const base = reinterpret_cast<byte*>(data.data());
    auto const pos = i64(curr - base);

    // bytes written past the committed end are pending but real, so they count towards the logical size
    auto const logical_size = cc::max(i64(data.size()), pos);

    switch (dir)
    {
    case seek_dir::dry_relative:
        return pos + offset; // pure query: no clamping, no mutation
    case seek_dir::dry_begin:
        return offset;
    case seek_dir::dry_end:
        return logical_size + offset;
    case seek_dir::remaining_size_hint:
        return -1; // a write sink has nothing still to come
    case seek_dir::relative:
    case seek_dir::begin:
    case seek_dir::end:
        break;
    }

    auto const target = dir == seek_dir::relative ? pos + offset
                      : dir == seek_dir::begin    ? offset
                                                  : logical_size + offset;
    if (target < 0 || target > logical_size)
        return cc::error("growing stream: seek out of range");

    // commit: the pending bytes already live in the spare capacity, so this only moves the size
    data.resize_to_uninitialized(isize(logical_size));
    data.reserve_back(k_min_window);

    // the reserve may have reallocated, so every pointer is recomputed from the new base
    auto* const new_base = reinterpret_cast<byte*>(data.data());
    curr = new_base + target;
    end = new_base + data.size() + data.capacity_back();
    write_end = end;
    return target;
}

template cc::result<i64> growing_adapter_flush<cc::vector<byte>>(byte*&, byte*&, byte*&, void*, i64, seek_dir, byte*);
template cc::result<i64> growing_adapter_flush<cc::string>(byte*&, byte*&, byte*&, void*, i64, seek_dir, byte*);
} // namespace cc::impl
