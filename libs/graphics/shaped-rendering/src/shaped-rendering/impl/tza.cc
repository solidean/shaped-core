#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <shaped-rendering/impl/tza.hh>

namespace sr::impl
{
namespace
{
/// A bounds-checked cursor over the blob.
///
/// Every read goes through it, because the blob is a FILE: an offset in it is untrusted input, and the difference
/// between a truncated download and a crash is that every one of these is checked.
struct cursor
{
    cc::span<cc::byte const> blob;
    i64 at = 0;
    bool bad = false;

    [[nodiscard]] bool has(i64 count) const { return !bad && at >= 0 && count >= 0 && at + count <= blob.size(); }

    void seek(i64 to)
    {
        if (to < 0 || to > blob.size())
            bad = true;
        else
            at = to;
    }

    template <class T>
    [[nodiscard]] T read()
    {
        auto value = T{};
        if (!has(i64(sizeof(T))))
        {
            bad = true;
            return value;
        }
        cc::memcpy(&value, blob.data() + at, sizeof(T));
        at += i64(sizeof(T));
        return value;
    }

    [[nodiscard]] cc::string read_chars(i64 count)
    {
        if (!has(count))
        {
            bad = true;
            return {};
        }
        auto out = cc::string(cc::string_view(reinterpret_cast<char const*>(blob.data() + at), size_t(count)));
        at += count;
        return out;
    }
};

// What the format states about itself, from OIDN's own reader.
constexpr u16 k_magic = 0x41D7;
constexpr u8 k_major_version = 2;

/// The largest single dimension this reader accepts.
/// It keeps each factor small; `element_count_within` is what keeps their product from overflowing.
constexpr i64 k_max_dim = i64(1) << 30;

/// The fewest bytes one table record can take: a name length, a rank, an element type and a data offset.
constexpr i64 k_min_record_bytes = 2 + 1 + 1 + 8;

/// The product of `dims`, or -1 as soon as it exceeds `limit`.
/// Checked per factor, because a rank of up to 255 dimensions of up to 2^30 each overflows any integer.
[[nodiscard]] i64 element_count_within(cc::span<i32 const> dims, i64 limit)
{
    auto count = i64(1);
    for (auto const d : dims)
    {
        if (i64(d) > limit / count)
            return -1;
        count *= i64(d);
    }
    return count;
}
} // namespace

i64 tza_tensor::element_count() const
{
    auto count = i64(1);
    for (auto const d : dims)
        count *= i64(d);
    return count;
}

cc::vector<tza_tensor> read_tza(cc::span<cc::byte const> blob)
{
    auto c = cursor{.blob = blob};

    if (c.read<u16>() != k_magic)
    {
        CC_LOG_WARNING("tza: not a tensor archive");
        return {};
    }

    auto const major = c.read<u8>();
    (void)c.read<u8>(); // minor, which the format does not gate on
    if (c.bad || major != k_major_version)
    {
        CC_LOG_WARNING("tza: version {} is not the 2.x this reader understands", major);
        return {};
    }

    // The table lives at the END of the blob, so its offset is the first thing that can point anywhere.
    c.seek(i64(c.read<u64>()));
    auto const count = i64(c.read<u32>());
    if (c.bad)
    {
        CC_LOG_WARNING("tza: the tensor table is not where the header says");
        return {};
    }

    // The count sizes an allocation below, so it is held to what the remaining bytes could possibly describe.
    if (count > (blob.size() - c.at) / k_min_record_bytes)
    {
        CC_LOG_WARNING("tza: the table claims {} tensors, more than its {} bytes could hold", count, blob.size() - c.at);
        return {};
    }

    auto out = cc::vector<tza_tensor>();
    out.reserve(count);

    for (auto i = i64(0); i < count; ++i)
    {
        auto tensor = tza_tensor{};

        tensor.name = c.read_chars(i64(c.read<u16>()));

        auto const rank = i64(c.read<u8>());
        tensor.dims.reserve(rank);
        for (auto d = i64(0); d < rank; ++d)
        {
            auto const dim = i64(c.read<u32>());
            if (dim <= 0 || dim > k_max_dim)
            {
                CC_LOG_WARNING("tza: tensor '{}' has an impossible dimension", tensor.name);
                return {};
            }
            tensor.dims.push_back(i32(dim));
        }

        tensor.layout = c.read_chars(rank);

        auto const element = c.read<char>();
        if (element == 'f')
            tensor.element = tza_element::float32;
        else if (element == 'h')
            tensor.element = tza_element::float16;
        else
        {
            CC_LOG_WARNING("tza: tensor '{}' has element type '{}', which is neither f nor h", tensor.name, element);
            return {};
        }

        auto const offset = i64(c.read<u64>());
        if (c.bad)
        {
            CC_LOG_WARNING("tza: the tensor table ends before it says it does");
            return {};
        }

        auto const stride = tensor.element == tza_element::float32 ? i64(4) : i64(2);
        auto const elements = element_count_within(tensor.dims, blob.size() / stride);
        if (elements < 0)
        {
            CC_LOG_WARNING("tza: tensor '{}' declares more elements than the blob holds", tensor.name);
            return {};
        }

        auto const bytes = elements * stride;
        if (offset < 0 || offset > blob.size() || bytes > blob.size() - offset)
        {
            CC_LOG_WARNING("tza: tensor '{}' claims bytes outside the blob", tensor.name);
            return {};
        }

        tensor.data = blob.subspan({.offset = offset, .size = bytes});
        out.push_back(cc::move(tensor));
    }

    return out;
}

tza_tensor const* find_tza(cc::span<tza_tensor const> tensors, cc::string_view name)
{
    for (auto const& t : tensors)
        if (t.name == name)
            return &t;
    return nullptr;
}
} // namespace sr::impl
