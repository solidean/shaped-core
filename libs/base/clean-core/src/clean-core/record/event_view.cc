#include "event_view.hh"

#include <clean-core/record/chunk.hh>
#include <clean-core/record/system.hh>

using namespace cc::primitive_defines;

namespace
{
/// Reads a fixed-width field out of `payload`, failing when it does not fit.
template <class T>
bool read_raw(cc::span<byte const> payload, cc::rec::field const& f, T& out)
{
    if (isize(f.offset) + isize(sizeof(T)) > payload.size())
        return false;

    cc::memcpy(&out, payload.data() + f.offset, sizeof(T));
    return true;
}

/// Reads a field stored as T and hands it back as R.
template <class T, class R>
cc::optional<R> read_as(cc::span<byte const> payload, cc::rec::field const& f)
{
    T value = {};
    if (!read_raw(payload, f, value))
        return {};
    return R(value);
}

cc::rec::field const* find_field(cc::rec::event_view const& e, cc::string_view name)
{
    for (auto const& f : e.fields())
        if (cc::string_view(f.name) == name)
            return &f;
    return nullptr;
}
} // namespace

cc::rec::event_view cc::rec::event_iterator::operator*() const
{
    auto const& header = *reinterpret_cast<rec::impl::event_header const*>(_cur);
    return {
        .desc = header.desc,
        .cycles = header.cycles,
        .core = header.core,
        .flags = header.flags,
        .payload = cc::span<byte const>(_cur + sizeof(rec::impl::event_header), isize(header.payload_size)),
    };
}

cc::rec::event_iterator& cc::rec::event_iterator::operator++()
{
    auto const& header = *reinterpret_cast<rec::impl::event_header const*>(_cur);
    _cur += rec::impl::event_bytes_for(isize(header.payload_size));
    return *this;
}

cc::optional<f64> cc::rec::event_view::field_as_double(cc::string_view field_name) const
{
    auto const* const f = find_field(*this, field_name);
    if (f == nullptr)
        return {};

    switch (f->type)
    {
    case rec::type_code::boolean:
        return read_as<u8, f64>(payload, *f);
    case rec::type_code::i8_:
        return read_as<i8, f64>(payload, *f);
    case rec::type_code::i16_:
        return read_as<i16, f64>(payload, *f);
    case rec::type_code::i32_:
        return read_as<i32, f64>(payload, *f);
    case rec::type_code::i64_:
        return read_as<i64, f64>(payload, *f);
    case rec::type_code::u8_:
        return read_as<u8, f64>(payload, *f);
    case rec::type_code::u16_:
        return read_as<u16, f64>(payload, *f);
    case rec::type_code::u32_:
        return read_as<u32, f64>(payload, *f);
    case rec::type_code::u64_:
        return read_as<u64, f64>(payload, *f);
    case rec::type_code::f32_:
        return read_as<f32, f64>(payload, *f);
    case rec::type_code::f64_:
        return read_as<f64, f64>(payload, *f);
    default:
        return {};
    }
}

cc::optional<i64> cc::rec::event_view::field_as_int(cc::string_view field_name) const
{
    auto const* const f = find_field(*this, field_name);
    if (f == nullptr)
        return {};

    switch (f->type)
    {
    case rec::type_code::boolean:
        return read_as<u8, i64>(payload, *f);
    case rec::type_code::i8_:
        return read_as<i8, i64>(payload, *f);
    case rec::type_code::i16_:
        return read_as<i16, i64>(payload, *f);
    case rec::type_code::i32_:
        return read_as<i32, i64>(payload, *f);
    case rec::type_code::i64_:
        return read_as<i64, i64>(payload, *f);
    case rec::type_code::u8_:
        return read_as<u8, i64>(payload, *f);
    case rec::type_code::u16_:
        return read_as<u16, i64>(payload, *f);
    case rec::type_code::u32_:
        return read_as<u32, i64>(payload, *f);
    case rec::type_code::u64_:
    {
        u64 v = 0;
        if (!read_raw(payload, *f, v) || v > u64(0x7FFF'FFFF'FFFF'FFFF))
            return {};
        return i64(v);
    }
    default:
        return {};
    }
}

cc::optional<cc::string_view> cc::rec::event_view::field_as_text(cc::string_view field_name) const
{
    auto const* const f = find_field(*this, field_name);
    if (f == nullptr)
        return {};

    if (f->type == rec::type_code::cstring)
    {
        // Eight bytes on every target, so it is read as a u64 and cast back rather than read as a pointer.
        u64 address = 0;
        if (!read_raw(payload, *f, address) || address == 0)
            return {};
        return cc::string_view(reinterpret_cast<char const*>(uintptr_t(address)));
    }

    if (f->type == rec::type_code::inline_text)
    {
        u32 size = 0;
        if (!read_raw(payload, *f, size))
            return {};

        auto const start = isize(f->offset) + isize(sizeof(u32));
        if (start + isize(size) > payload.size())
            return {};
        return cc::string_view(reinterpret_cast<char const*>(payload.data()) + start, isize(size));
    }

    return {};
}

cc::optional<u64> cc::rec::event_view::field_as_u64(cc::string_view field_name) const
{
    auto const* const f = find_field(*this, field_name);
    if (f == nullptr)
        return {};

    switch (f->type)
    {
    case rec::type_code::u8_:
        return read_as<u8, u64>(payload, *f);
    case rec::type_code::u16_:
        return read_as<u16, u64>(payload, *f);
    case rec::type_code::u32_:
        return read_as<u32, u64>(payload, *f);
    case rec::type_code::u64_:
        return read_as<u64, u64>(payload, *f);
    case rec::type_code::pointer:
        // An address is an identity here, not a number — which is exactly why it reads as the widest integer and
        // never as a double.
        return read_as<u64, u64>(payload, *f);
    default:
        return {};
    }
}

cc::string_view cc::rec::event_view::name() const
{
    // The overwhelmingly common case first, and it is a load and a branch: a static name is never empty.
    if (desc->name != nullptr && desc->name[0] != char(0))
        return desc->name;

    if (auto const dynamic = field_as_text("name"); dynamic.has_value())
        return dynamic.value();

    return {};
}

cc::span<byte const> cc::rec::event_view::field_as_bytes(cc::string_view field_name) const
{
    auto const* const f = find_field(*this, field_name);
    if (f == nullptr || f->type != rec::type_code::pinned_bytes)
        return {};

    // Both halves are u64: the address slot is eight bytes on every target, wasm32's four-byte pointers included.
    u64 address = 0;
    if (!read_raw(payload, *f, address) || address == 0)
        return {};

    // The size sits in the next eight bytes, which is what the pinned layout declares.
    // Read through a field rather than assumed, so a payload too short for it reads as empty.
    u64 size = 0;
    auto const size_field = rec::field{.name = "", .type = rec::type_code::u64_, .offset = u16(f->offset + 8), .size = 8};
    if (!read_raw(payload, size_field, size))
        return {};

    return cc::span<byte const>(reinterpret_cast<byte const*>(uintptr_t(address)), isize(size));
}

cc::rec::desc const* cc::rec::event_view::field_as_desc(cc::string_view field_name) const
{
    auto const* const f = find_field(*this, field_name);
    if (f == nullptr || f->type != rec::type_code::desc_ref)
        return nullptr;

    // Read as a u64, never as a pointer: the slot is eight bytes on every target, and wasm32's pointers are four.
    u64 target = 0;
    if (!read_raw(payload, *f, target))
        return nullptr;

    return reinterpret_cast<rec::desc const*>(uintptr_t(target));
}

cc::vector<u64> cc::rec::event_view::field_as_u64_array(cc::string_view field_name) const
{
    cc::vector<u64> out;

    auto const* const f = find_field(*this, field_name);
    if (f == nullptr || f->type != rec::type_code::u64_array)
        return out;

    u32 count = 0;
    if (!read_raw(payload, *f, count))
        return out;

    auto const start = isize(f->offset) + isize(sizeof(u32));
    if (start + isize(count) * isize(sizeof(u64)) > payload.size())
        return out;

    out.reserve(isize(count));
    for (u32 i = 0; i < count; ++i)
    {
        u64 value = 0;
        cc::memcpy(&value, payload.data() + start + isize(i) * isize(sizeof(u64)), sizeof(value));
        out.push_back(value);
    }
    return out;
}

f64 cc::rec::chunk_view::wall_secs_of(u64 cycles) const
{
    // A sealed chunk carries both ends, so the mapping is exact over its own span and needs no global calibration.
    if (seal_wall_secs > 0 && seal_cycles > base_cycles)
    {
        auto const t = f64(cycles - base_cycles) / f64(seal_cycles - base_cycles);
        return base_wall_secs + t * (seal_wall_secs - base_wall_secs);
    }

    auto const rate = rec::cycles_per_second();
    if (rate <= 0)
        return base_wall_secs;

    return base_wall_secs + f64(i64(cycles - base_cycles)) / rate;
}
