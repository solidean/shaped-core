#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/archive.hh>
#include <shaped-graphics-language/check/impl/fields.hh>
#include <shaped-graphics-language/check/impl/relocate.hh>

#include <type_traits>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;


void checker::note_body_read(symbol_id function) const
{
    if (!records_interface || !is_valid(function))
        return;
    auto const file = out.at(function).file;
    if (is_prelude_file(file) || file >= run_from_files)
        return;
    for (auto i = bodies_read_from; i < bodies_read.size(); ++i)
        if (bodies_read[i] == function)
            return;
    bodies_read.push_back(function);
}

namespace
{
/// Where the parts of the links `old_chain` names lie in `new_chain`: each kept whole where it checked into the same,
/// its interface where that hashes the same, and nothing otherwise.
/// None where the chains are not of the same modules with the same files, or where `bodies` holds a function of a link
/// not kept whole.
cc::optional<cc::vector<moved_part>> moved_parts(cc::span<checker::chain_part const> old_chain,
                                                 cc::span<checker::chain_part const> new_chain,
                                                 cc::span<symbol_id const> bodies)
{
    if (old_chain.size() != new_chain.size())
        return {};
    auto parts = cc::vector<moved_part>();
    for (auto i = isize(0); i < old_chain.size(); ++i)
    {
        auto const& b = old_chain[i];
        auto const& n = new_chain[i];
        // a symbol names its file by position, which the same modules' files in the same order keep
        if (b.module != n.module || b.files != n.files || (i == 0 && b.start != n.start))
            return {};
        auto part = moved_part{.old_start = b.start, .old_end = b.end, .new_start = n.start};
        if (b.part_digest == n.part_digest && same_shape(b.start, b.end, n.start, n.end))
            part.old_kept = b.end;
        else if (b.interface_digest == n.interface_digest
                 && same_shape(b.start, b.interface_end, n.start, n.interface_end))
            part.old_kept = b.interface_end;
        else
            part.old_kept = b.start;
        // a body of a link whose sources changed is what the link inlined or instantiated, and may read otherwise now
        if (part.old_kept != b.end || b.sources_digest != n.sources_digest)
            for (auto const f : bodies)
                if (index_of(f) >= b.start.symbols && index_of(f) < b.end.symbols)
                    return {};
        // the parts before the first that moved map every id to itself
        if (parts.empty() && part.old_start == part.new_start && part.old_kept == part.old_end)
            continue;
        parts.push_back(part);
    }
    return parts;
}
} // namespace

cc::vector<cc::string_view> impl::layer_maker::own_sources(cc::span<module_file const> files, i32 prelude, isize own_from)
{
    auto out = cc::vector<cc::string_view>();
    for (auto i = isize(0); i < files.size(); ++i)
        if (i < prelude || i >= own_from)
            out.push_back(cc::string_view(files[i].file.source));
    return out;
}

cc::hash128 impl::layer_maker::sources_digest(cc::span<module_file const> files, isize own_from)
{
    auto h = cc::hash128::create({}, u64(files.size() - own_from));
    for (auto i = own_from; i < files.size(); ++i)
        h = cc::hash128::create(cc::string_view(files[i].file.source).as_bytes(), h.low ^ (h.high * 31));
    return h;
}

cc::vector<cc::string_view> impl::layer_maker::sources_between(cc::span<module_file const> files,
                                                               i32 prelude,
                                                               isize own_from)
{
    auto out = cc::vector<cc::string_view>();
    for (auto i = isize(prelude); i < own_from; ++i)
        out.push_back(cc::string_view(files[i].file.source));
    return out;
}

bool impl::layer_maker::move_onto(checked_prelude& layer,
                                  checked_prelude const& new_base,
                                  archive<false>& a,
                                  cc::span<checker::chain_part const> old_chain,
                                  checker::chain_part const& own,
                                  cc::span<symbol_id const> bodies)
{
    auto const& base = *new_base._state;
    auto parts = moved_parts(old_chain, base.chain, bodies);
    if (!parts.has_value())
        return false;
    auto const start = sizes_of(base);
    if (old_chain.empty() && own.start != start)
        return false;
    // the link's own part moves as a whole, behind the new base
    parts.value().push_back({.old_start = own.start, .old_kept = own.end, .old_end = own.end, .new_start = start});

    auto state = cc::make_unique<checker>(base);
    if (!read_own_part(a, *state, parts.value()) || state->chain.size() != base.chain.size() + 1)
        return false;

    // the link's own part starts where the new base ends, its interface as long as before
    state->run_start = start;
    state->interface_end = plus(start, own.start, own.interface_end);
    // what the interface hashes: its entries as they now stand, which a body checked after it never writes but where it
    // moved (`archive::io(function_info&)`)
    state->interface_digest = interface_hash(*state, state->run_start, state->interface_end);
    state->part_digest = interface_hash(*state, state->run_start, sizes_of(*state));
    state->bodies_read_from = base.bodies_read.size();
    state->files = {};
    state->chain.back() = {
        .module = own.module,
        .start = state->run_start,
        .interface_end = state->interface_end,
        .end = sizes_of(*state),
        .files = i32(layer._files.size()),
        .interface_digest = state->interface_digest,
        .part_digest = state->part_digest,
        .sources_digest = own.sources_digest,
    };
    layer._is_relocated = true;
    layer._resume = seal_layer(*state, i32(layer._files.size()), true);
    layer._state = cc::move(state);
    return true;
}

cc::optional<checked_prelude> sgl::check::relocate_link(checked_prelude const& link,
                                                        checked_prelude const& old_base,
                                                        checked_prelude const& new_base)
{
    CC_ASSERT(link._is_library, "relocation moves links of a chain");
    auto const& own = *link._state;
    if (!link._options.empty() || own.chain.empty())
        return {};
    auto const old_files = old_base._files.size();

    // the link's own part, written over the old base and read back over the new one
    auto bytes = cc::vector<byte>();
    bytes.reserve(isize(1) << 16);
    auto const sources = impl::layer_maker::own_sources(link._files, link._prelude_count, old_files);
    auto const old_between = impl::layer_maker::sources_between(link._files, link._prelude_count, old_files);
    auto w = archive<true>{.out = &bytes, .sources = sources, .elsewhere = old_between};
    impl::write_own_part(w, own, *old_base._state);
    if (w.failed)
        return {};

    auto result = checked_prelude();
    result._files.push_back_range(cc::span<module_file const>(new_base._files));
    for (auto i = old_files; i < link._files.size(); ++i)
        result._files.push_back(link._files[i]);
    result._prelude_count = link._prelude_count;
    result._library_order = link._library_order;
    result._modules = link._modules;
    result._is_library = true;
    result._options = new_base._options;
    result._names_by_reach = cc::make_unique<cc::mutex<cc::map<cc::string, cc::shared_ptr<cc::set<cc::string>>>>>();
    auto const new_between
        = impl::layer_maker::sources_between(new_base._files, new_base._prelude_count, new_base._files.size());
    auto r = archive<false>{
        .in = bytes,
        .sources = sources,
        .elsewhere = new_between,
        .texts = &result._texts,
        .registry = &own.builtins,
    };
    auto const old_chain
        = cc::span<impl::checker::chain_part const>(own.chain).subspan({.offset = 0, .size = own.chain.size() - 1});
    auto const bodies
        = cc::span<symbol_id const>(own.bodies_read)
              .subspan({.offset = own.bodies_read_from, .size = own.bodies_read.size() - own.bodies_read_from});
    if (!impl::layer_maker::move_onto(result, new_base, r, old_chain, own.chain.back(), bodies))
        return {};
    return result;
}
