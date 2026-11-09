#include <clean-core/algorithm/sort.hh>
#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <shaped-graphics-language/check/artifact.hh>
#include <shaped-graphics-language/check/impl/archive.hh>
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
cc::vector<byte> framed(cc::span<byte const> payload, cc::string_view key, cc::span<cc::string_view const> sources)
{
    auto h = header{};
    cc::memcpy(h.magic, k_magic, sizeof(k_magic));
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
bool is_framed(cc::span<byte const> bytes, cc::string_view key, cc::span<cc::string_view const> sources)
{
    if (bytes.size() < isize(sizeof(header)))
        return false;
    auto h = header{};
    cc::memcpy(&h, bytes.data(), sizeof(header));
    auto const payload = bytes.subspan({.offset = isize(sizeof(header)), .size = bytes.size() - isize(sizeof(header))});
    return cc::memcmp(h.magic, k_magic, sizeof(k_magic)) == 0
        && h.key_hash == cc::hash128::create(key.as_bytes(), 0).low && h.sources == sources_hash(sources)
        && h.payload_size == u64(payload.size()) && h.payload == cc::hash128::create(payload, h.payload_size);
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
