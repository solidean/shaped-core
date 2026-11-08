#include <clean-core/algorithm/sort.hh>
#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <shaped-graphics-language/check/artifact.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/impl/fields.hh>

#include <type_traits>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// What every artifact starts with; a different one is a miss.
constexpr char k_magic[8] = {'S', 'G', 'L', 'L', 'A', 'Y', 'E', 'R'};

/// The entries of a map in the order of their keys, so a layer is the same bytes whatever order its maps were filled in.
template <class Key, class Value, class Map>
cc::vector<cc::pair<Key const*, Value const*>> sorted_entries(Map const& m)
{
    auto out = cc::vector<cc::pair<Key const*, Value const*>>();
    for (auto const& [key, value] : m)
        out.push_back({&key, &value});
    cc::sort(out, [](auto const& a, auto const& b) { return *a.first < *b.first; });
    return out;
}

/// One pass over a checked layer, writing it or reading it back: the same walk both ways, so the two cannot disagree.
///
/// A field is written by its type: a number or an enum as its bytes, a container as its size and then its elements,
/// and an aggregate as each of its fields in turn (`for_each_field`).
/// A string view is the one value that names memory, so it is written as a place in one of the files' sources where it
/// is one, and as its own text otherwise, which a read keeps in `texts`.
/// A pointer and a span are a layer's only when they are empty, and the registry is the caller's, so neither is written.
template <bool Writing>
struct archive
{
    /// Writing: where the bytes go.
    cc::vector<byte>* out = nullptr;
    /// Reading: the bytes, and how far the walk got.
    cc::span<byte const> in;
    isize at = 0;
    /// The sources of the layer's files, which a string view of the layer points into.
    cc::span<cc::string_view const> sources;
    /// Reading: where a string view in none of `sources` keeps its text.
    cc::vector<cc::unique_ptr<cc::string>>* texts = nullptr;
    builtins::registry const* registry = nullptr;
    /// Writing: what the layer holds that no artifact carries; reading: the bytes ran out or held nonsense.
    bool failed = false;

    void raw(void* data, isize size)
    {
        if constexpr (Writing)
        {
            auto const* const bytes = static_cast<byte const*>(data);
            for (auto i = isize(0); i < size; ++i)
                out->push_back(bytes[i]);
        }
        else
        {
            if (failed || at + size > in.size())
            {
                failed = true;
                cc::memset(data, 0, size);
                return;
            }
            cc::memcpy(data, in.data() + at, size);
            at += size;
        }
    }

    /// The size of a container: written as is, and read only as far as the bytes left could hold.
    isize count(isize n)
    {
        auto v = i64(n);
        raw(&v, sizeof(v));
        if constexpr (!Writing)
            if (v < 0 || v > in.size() - at)
            {
                failed = true;
                return 0;
            }
        return isize(v);
    }

    void io(cc::string& s)
    {
        auto const n = count(s.size());
        if constexpr (Writing)
            raw(const_cast<char*>(s.data()), n);
        else
        {
            s = cc::string::create_uninitialized(n);
            raw(s.data(), n);
        }
    }

    void io(cc::string_view& v)
    {
        // 0: empty; 1: a place in a source; 2: its own text
        auto tag = u8(0);
        auto source = i32(-1);
        auto offset = i64(0);
        auto size = i64(v.size());
        if constexpr (Writing)
        {
            if (!v.empty())
            {
                tag = 2;
                for (auto i = isize(0); i < sources.size(); ++i)
                    if (v.data() >= sources[i].data() && v.data() + v.size() <= sources[i].data() + sources[i].size())
                    {
                        tag = 1;
                        source = i32(i);
                        offset = v.data() - sources[i].data();
                        break;
                    }
            }
        }
        raw(&tag, sizeof(tag));
        if (tag == 0)
        {
            if constexpr (!Writing)
                v = {};
            return;
        }
        if (tag == 1)
        {
            raw(&source, sizeof(source));
            raw(&offset, sizeof(offset));
            raw(&size, sizeof(size));
            if constexpr (!Writing)
            {
                if (failed || source < 0 || source >= sources.size() || offset < 0 || size < 0
                    || offset + size > sources[source].size())
                {
                    failed = true;
                    v = {};
                    return;
                }
                v = sources[source].subview({.offset = offset, .size = size});
            }
            return;
        }
        if constexpr (Writing)
        {
            auto text = cc::string(v);
            io(text);
        }
        else
        {
            auto text = cc::make_unique<cc::string>();
            io(*text);
            v = cc::string_view(*text);
            texts->push_back(cc::move(text));
        }
        if (tag > 2)
            failed = true;
    }

    template <class T>
    void io(cc::vector<T>& v)
    {
        auto const n = count(v.size());
        if constexpr (!Writing)
        {
            v.clear();
            v.resize_to_defaulted(n);
        }
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            raw(v.data(), n * isize(sizeof(T)));
        else
            for (auto i = isize(0); i < n; ++i)
                io(v[i]);
    }

    template <class T>
    void io(layered<T>& v)
    {
        auto const n = count(v.size());
        if constexpr (Writing)
            for (auto i = isize(0); i < n; ++i)
                io(const_cast<T&>(v[i]));
        else
        {
            v = {};
            for (auto i = isize(0); i < n && !failed; ++i)
            {
                auto e = T();
                io(e);
                v.push_back(cc::move(e));
            }
            v.freeze();
        }
    }

    template <class K, class V>
    void io(cc::map<K, V>& m)
    {
        auto const n = count(m.size());
        if constexpr (Writing)
            for (auto const& [key, value] : sorted_entries<K, V>(m))
            {
                io(const_cast<K&>(*key));
                io(const_cast<V&>(*value));
            }
        else
        {
            m.clear();
            for (auto i = isize(0); i < n && !failed; ++i)
            {
                auto key = K();
                auto value = V();
                io(key);
                io(value);
                m[key] = cc::move(value);
            }
        }
    }

    template <class K>
    void io(cc::set<K>& s)
    {
        auto const n = count(s.size());
        if constexpr (Writing)
        {
            auto keys = cc::vector<K const*>();
            for (auto const& key : s)
                keys.push_back(&key);
            cc::sort(keys, [](K const* a, K const* b) { return *a < *b; });
            for (auto const* key : keys)
                io(const_cast<K&>(*key));
        }
        else
        {
            s.clear();
            for (auto i = isize(0); i < n && !failed; ++i)
            {
                auto key = K();
                io(key);
                (void)s.insert(cc::move(key));
            }
        }
    }

    template <class K, class V>
    void io(layered_map<K, V>& m)
    {
        auto n = isize(0);
        if constexpr (Writing)
            for (auto const& entry : m)
                (void)entry, ++n;
        n = count(n);
        if constexpr (Writing)
            for (auto const& [key, value] : sorted_entries<K, V>(m))
            {
                io(const_cast<K&>(*key));
                io(const_cast<V&>(*value));
            }
        else
        {
            m = {};
            for (auto i = isize(0); i < n && !failed; ++i)
            {
                auto key = K();
                auto value = V();
                io(key);
                io(value);
                m[key] = cc::move(value);
            }
            m.freeze();
        }
    }

    template <class T>
    void io(cc::optional<T>& o)
    {
        auto has = o.has_value();
        raw(&has, sizeof(has));
        if constexpr (Writing)
        {
            if (has)
                io(o.value());
        }
        else if (has)
        {
            auto value = T();
            io(value);
            o = cc::move(value);
        }
        else
            o = {};
    }

    template <class T>
    void io(cc::shared_ptr<T>& p)
    {
        using value_type = std::remove_const_t<T>;
        auto has = p != nullptr;
        raw(&has, sizeof(has));
        if constexpr (Writing)
        {
            if (has)
                io(const_cast<value_type&>(*p));
        }
        else if (has)
        {
            auto value = value_type();
            io(value);
            p = cc::make_shared<value_type>(cc::move(value));
        }
        else
            p = nullptr;
    }

    template <class T, isize N>
    void io(cc::fixed_array<T, N>& a)
    {
        for (auto i = isize(0); i < N; ++i)
            io(a[i]);
    }

    /// The caller's views, a layer's only while it is empty: the files, the options.
    template <class T>
    void io(cc::span<T>& s)
    {
        if constexpr (Writing)
            failed = failed || !s.empty();
        else
            s = {};
    }

    /// The registry, which a load takes from its caller.
    void io(builtins::registry const*& p)
    {
        if constexpr (!Writing)
            p = registry;
    }
    void io(builtins::registry const&) {}

    /// Any other pointer is a layer's only while it is null.
    template <class T>
    void io(T*& p)
    {
        if constexpr (Writing)
            failed = failed || p != nullptr;
        else
            p = nullptr;
    }

    /// A flat tree is no artifact's yet: a layer holding one is not written.
    void io(flat_entry_point&) { failed = true; }

    template <class T>
    void io(T& v)
    {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            raw(&v, sizeof(T));
        else if constexpr (std::is_aggregate_v<T>)
            for_each_field(v, [this](auto& field) { io(field); });
        else
        {
            static_assert(std::is_trivially_copyable_v<T>, "a type the artifact has no way to write");
            raw(&v, sizeof(T));
        }
    }
};

/// Everything a layer is but its files, which a load takes from its caller.
template <bool Writing>
void walk(archive<Writing>& a, i32& prelude_count, resume_point& resume, checker& c)
{
    a.io(prelude_count);
    a.io(resume);
    a.io(c);
}

/// What an artifact is keyed on besides its caller's key: the shape of what it holds, and the prelude's sources.
cc::hash128 sources_hash(cc::span<cc::string_view const> sources)
{
    // a build with other assertions or another word size lays out what the walk reads differently
    auto h = cc::hash128::create({}, sizeof(checker) * 4 + sizeof(void*) + (CC_ASSERT_ENABLED ? 1 : 0));
    for (auto const& s : sources)
        h = cc::hash128::create(s.as_bytes(), h.low ^ (h.high * 31) ^ u64(s.size()));
    return h;
}

cc::vector<cc::string_view> sources_of(cc::span<module_file const> files)
{
    auto out = cc::vector<cc::string_view>();
    for (auto const& f : files)
        out.push_back(cc::string_view(f.file.source));
    return out;
}

struct header
{
    char magic[8] = {};
    u64 key_hash = 0;
    cc::hash128 sources;
    u64 payload_size = 0;
    cc::hash128 payload;
};
} // namespace

cc::optional<cc::vector<byte>> sgl::check::save_artifact(checked_prelude const& layer, cc::string_view key)
{
    if (!layer._library_order.empty())
        return {};
    auto const sources = sources_of(layer._files);
    auto payload = cc::vector<byte>();
    auto a = archive<true>{.out = &payload, .sources = sources};
    walk(a, const_cast<i32&>(layer._prelude_count), const_cast<resume_point&>(layer._resume),
         const_cast<checker&>(*layer._state));
    if (a.failed)
        return {};

    auto h = header{};
    cc::memcpy(h.magic, k_magic, sizeof(k_magic));
    h.key_hash = cc::hash128::create(key.as_bytes(), 0).low;
    h.sources = sources_hash(sources);
    h.payload_size = u64(payload.size());
    h.payload = cc::hash128::create(payload, h.payload_size);
    auto out = cc::vector<byte>();
    out.reserve(isize(sizeof(header)) + payload.size());
    auto const* const head = reinterpret_cast<byte const*>(&h);
    for (auto i = isize(0); i < isize(sizeof(header)); ++i)
        out.push_back(head[i]);
    out.push_back_range(payload);
    return out;
}

cc::optional<checked_prelude> sgl::check::load_artifact(cc::span<byte const> bytes,
                                                        cc::string_view key,
                                                        cc::span<module_file const> prelude,
                                                        builtins::registry const& builtins)
{
    if (bytes.size() < isize(sizeof(header)))
        return {};
    auto h = header{};
    cc::memcpy(&h, bytes.data(), sizeof(header));
    auto const payload = bytes.subspan({.offset = isize(sizeof(header)), .size = bytes.size() - isize(sizeof(header))});
    auto const sources = sources_of(prelude);
    if (cc::memcmp(h.magic, k_magic, sizeof(k_magic)) != 0 || h.key_hash != cc::hash128::create(key.as_bytes(), 0).low
        || !(h.sources == sources_hash(sources)) || h.payload_size != u64(payload.size())
        || !(h.payload == cc::hash128::create(payload, h.payload_size)))
        return {};

    auto result = checked_prelude();
    result._files.push_back_range(prelude);
    result._state = cc::make_unique<checker>(checker{.files = {}, .builtins = builtins});
    auto a = archive<false>{.in = payload, .sources = sources, .texts = &result._texts, .registry = &builtins};
    walk(a, result._prelude_count, result._resume, *result._state);
    if (a.failed || a.at != payload.size() || result._prelude_count != i32(prelude.size()))
        return {};
    return result;
}
