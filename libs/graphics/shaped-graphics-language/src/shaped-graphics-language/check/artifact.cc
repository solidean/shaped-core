#include <clean-core/algorithm/sort.hh>
#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/artifact.hh>
#include <shaped-graphics-language/check/impl/archive.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/impl/fields.hh>
#include <shaped-graphics-language/check/impl/relocate.hh>

#include <type_traits>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// What every artifact starts with; a different one is a miss.
constexpr char k_magic[8] = {'S', 'G', 'L', 'L', 'A', 'Y', 'E', 'R'};
/// What an artifact of one link of a chain starts with instead (`save_link_artifact`).
constexpr char k_link_magic[8] = {'S', 'G', 'L', 'L', 'I', 'N', 'K', '1'};

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

/// The header followed by `payload`, which is what an artifact is.
cc::vector<byte> framed(cc::span<byte const> payload,
                        cc::string_view key,
                        cc::span<cc::string_view const> sources,
                        char const (&magic)[8] = k_magic)
{
    auto h = header{};
    cc::memcpy(h.magic, magic, sizeof(magic));
    h.key_hash = cc::hash128::create(key.as_bytes(), 0).low;
    h.sources = sources_hash(sources);
    h.payload_size = u64(payload.size());
    h.payload = cc::hash128::create(payload, h.payload_size);
    auto out = cc::vector<byte>();
    out.reserve(isize(sizeof(header)) + payload.size());
    out.push_back_range(cc::span<byte const>(reinterpret_cast<byte const*>(&h), isize(sizeof(header))));
    out.push_back_range(payload);
    return out;
}

/// Whether `bytes` are an artifact written under `key` for `sources`, whole and unchanged.
bool is_framed(cc::span<byte const> bytes,
               cc::string_view key,
               cc::span<cc::string_view const> sources,
               char const (&magic)[8] = k_magic)
{
    if (bytes.size() < isize(sizeof(header)))
        return false;
    auto h = header{};
    cc::memcpy(&h, bytes.data(), sizeof(header));
    auto const payload = bytes.subspan({.offset = isize(sizeof(header)), .size = bytes.size() - isize(sizeof(header))});
    return cc::memcmp(h.magic, magic, sizeof(magic)) == 0 && h.key_hash == cc::hash128::create(key.as_bytes(), 0).low
        && h.sources == sources_hash(sources) && h.payload_size == u64(payload.size())
        && h.payload == cc::hash128::create(payload, h.payload_size);
}
} // namespace

cc::optional<cc::vector<byte>> sgl::check::save_artifact(checked_prelude const& layer, cc::string_view key)
{
    auto const sources = sources_of(layer._files);
    auto payload = cc::vector<byte>();
    auto a = archive<true>{.out = &payload, .sources = sources};
    walk(a, const_cast<i32&>(layer._prelude_count), const_cast<resume_point&>(layer._resume),
         const_cast<checker&>(*layer._state));
    if (a.failed)
        return {};
    return framed(payload, key, sources);
}

cc::optional<checked_prelude> sgl::check::load_artifact(cc::span<byte const> bytes,
                                                        cc::string_view key,
                                                        cc::span<module_file const> prelude,
                                                        builtins::registry const& builtins)
{
    auto const sources = sources_of(prelude);
    if (!is_framed(bytes, key, sources))
        return {};
    auto const payload = bytes.subspan({.offset = isize(sizeof(header)), .size = bytes.size() - isize(sizeof(header))});
    auto result = checked_prelude();
    result._files.push_back_range(prelude);
    result._state = cc::make_unique<checker>(checker{.files = {}, .builtins = builtins});
    auto a = archive<false>{.in = payload, .sources = sources, .texts = &result._texts, .registry = &builtins};
    walk(a, result._prelude_count, result._resume, *result._state);
    // a layer of modules has more files than the prelude, and reads back only through `load_modules_artifact`
    if (a.failed || a.at != payload.size() || result._prelude_count != i32(prelude.size())
        || result._resume.files != i32(prelude.size()))
        return {};
    return result;
}

cc::optional<checked_prelude> sgl::check::load_modules_artifact(cc::span<byte const> bytes,
                                                                cc::string_view key,
                                                                checked_prelude const& prelude,
                                                                cc::span<module_file const> library,
                                                                module_file program,
                                                                builtins::registry const& builtins)
{
    CC_ASSERT(prelude._library_order.empty(), "a layer of modules stands on the prelude's layer");
    // what the program reaches, planned as `check_modules` plans it: the same files in the same order are the same ids
    auto plan = plan_modules(library, program);
    if (!may_layer(plan))
        return {};
    auto files = cc::vector<module_file>();
    files.push_back_range(cc::span<module_file const>(prelude._files));
    for (auto const i : plan.library_order)
        files.push_back(library[i]);
    auto const sources = sources_of(files);
    if (!is_framed(bytes, key, sources))
        return {};

    auto const payload = bytes.subspan({.offset = isize(sizeof(header)), .size = bytes.size() - isize(sizeof(header))});
    auto result = checked_prelude();
    result._files = files;
    result._library_order = plan.library_order;
    result._modules = plan.modules;
    result._state = cc::make_unique<checker>(checker{.files = {}, .builtins = builtins});
    auto a = archive<false>{.in = payload, .sources = sources, .texts = &result._texts, .registry = &builtins};
    walk(a, result._prelude_count, result._resume, *result._state);
    if (a.failed || a.at != payload.size() || result._prelude_count != prelude._prelude_count
        || result._resume.files != i32(files.size()))
        return {};
    return result;
}

cc::optional<cc::vector<byte>> sgl::check::save_link_artifact(checked_prelude const& link,
                                                              checked_prelude const& base,
                                                              cc::string_view key)
{
    auto const& own = *link._state;
    if (!link._is_library || !link._options.empty() || own.chain.empty())
        return {};
    auto const base_files = base._files.size();
    auto const sources = impl::layer_maker::own_sources(link._files, link._prelude_count, base_files);
    auto const between = impl::layer_maker::sources_between(link._files, link._prelude_count, base_files);
    auto payload = cc::vector<byte>();
    auto a = archive<true>{.out = &payload, .sources = sources, .elsewhere = between};
    // the module, the parts of the chain up to its own, and the bodies of those it read: what a load checks the chain
    // it lands on against
    auto module = own.chain.back().module;
    a.io(module);
    auto chain = own.chain;
    a.io(chain);
    auto bodies = cc::vector<symbol_id>();
    for (auto i = own.bodies_read_from; i < own.bodies_read.size(); ++i)
        bodies.push_back(own.bodies_read[i]);
    a.io(bodies);
    impl::write_own_part(a, own, *base._state);
    if (a.failed)
        return {};
    return framed(payload, key, sources, k_link_magic);
}

cc::optional<checked_prelude> sgl::check::load_link_artifact(cc::span<byte const> bytes,
                                                             cc::string_view key,
                                                             checked_prelude const& base,
                                                             cc::span<module_file const> library,
                                                             cc::string_view module)
{
    if (!base._options.empty())
        return {};
    auto next = impl::layer_maker::plan_next(base, library, module);
    if (!next.has_value())
        return {};
    auto result = cc::move(next.value().layer);
    auto const base_files = base._files.size();
    auto const sources = impl::layer_maker::own_sources(result._files, result._prelude_count, base_files);
    if (!is_framed(bytes, key, sources, k_link_magic))
        return {};

    auto const payload = bytes.subspan({.offset = isize(sizeof(header)), .size = bytes.size() - isize(sizeof(header))});
    auto const between = impl::layer_maker::sources_between(base._files, base._prelude_count, base_files);
    auto a = archive<false>{
        .in = payload,
        .sources = sources,
        .elsewhere = between,
        .texts = &result._texts,
        .registry = &base._state->builtins,
    };
    auto saved_module = cc::string();
    a.io(saved_module);
    auto chain = cc::vector<impl::checker::chain_part>();
    a.io(chain);
    auto bodies = cc::vector<symbol_id>();
    a.io(bodies);
    if (a.failed || saved_module != module || chain.empty() || chain.back().module != module)
        return {};
    auto const old_chain
        = cc::span<impl::checker::chain_part const>(chain).subspan({.offset = 0, .size = chain.size() - 1});
    if (!impl::layer_maker::move_onto(result, base, a, old_chain, chain.back(), bodies))
        return {};
    return result;
}

namespace
{
/// Finds where two values of one type first differ, as written by the artifact's walk, and names the path there.
struct differ
{
    cc::string path;

    template <class T>
    cc::vector<byte> bytes_of(T const& v)
    {
        auto out = cc::vector<byte>();
        auto a = archive<true>{.out = &out, .views_as_text = true};
        a.io(const_cast<T&>(v));
        return out;
    }

    template <class T>
    bool same(T const& a, T const& b)
    {
        auto const x = bytes_of(a);
        auto const y = bytes_of(b);
        if (x.size() != y.size())
            return false;
        for (auto i = isize(0); i < x.size(); ++i)
            if (x[i] != y[i])
                return false;
        return true;
    }

    /// Whether `a` and `b` differ, `path` naming the first place where.
    template <class T>
    bool differs(T const& a, T const& b)
    {
        if constexpr (std::is_same_v<T, builtins::registry const> || std::is_same_v<T, builtins::registry>)
            return false;
        else
        {
            if (same(a, b))
                return false;
            if constexpr (is_layered_v<T> || is_vector_v<T> || is_fixed_array_v<T>)
            {
                if (a.size() != b.size())
                {
                    path.appendf(" size {} against {}", a.size(), b.size());
                    return true;
                }
                for (auto i = isize(0); i < a.size(); ++i)
                    if (path.appendf("[{}]", i); differs(a[i], b[i]))
                        return true;
                    else
                        path.resize_down_to(path.rfind('['));
                return true;
            }
            else if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>)
            {
                auto index = 0;
                auto found = false;
                for_each_field(a,
                               [&](auto const& field)
                               {
                                   if (found)
                                       return;
                                   auto const& other = same_member(field, a, b);
                                   auto const mark = path.size();
                                   path.appendf(".{}", index++);
                                   if (differs(field, other))
                                       found = true;
                                   else
                                       path.resize_down_to(mark);
                               });
                return true;
            }
            else
            {
                if constexpr (std::is_arithmetic_v<T>)
                    path.appendf(" = {} against {}", a, b);
                else if constexpr (std::is_enum_v<T>)
                    path.appendf(" = {} against {}", i64(a), i64(b));
                return true;
            }
        }
    }
};
} // namespace

cc::string sgl::check::layer_difference(checked_prelude const& a, checked_prelude const& b)
{
    auto d = differ{};
    if (!d.differs(*a._state, *b._state))
        return {};
    return cc::move(d.path);
}
