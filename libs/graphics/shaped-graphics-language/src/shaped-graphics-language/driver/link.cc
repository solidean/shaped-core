#include "link.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/driver/impl/describe_binding.hh>
#include <shaped-graphics-language/driver/impl/emit_text.hh>
#include <shaped-graphics-language/driver/impl/front_end.hh>
#include <shaped-graphics-language/driver/prelude.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

using namespace sgl;

namespace
{
/// One file of the group with the options its entries are built with, checked once to number and once per target.
/// Held by pointer, since each front end names the file by the `source_name` it owns.
struct linked_file
{
    cc::string source_name;
    cc::vector<check::option_value> options;
    driver::impl::front_end numbered;
    /// Parallel to `state::targets`, each with the group's numbering of its own symbols.
    cc::vector<driver::impl::front_end> written;
    cc::vector<emit::numbering> numberings;
};

struct linked_entry
{
    i32 file = -1;
    cc::string entry_point;
    check::stage stage = check::stage::none;
    /// `file:entry`, and `:name=value` per option.
    cc::string name;
    u64 hash = 0;
};

/// What a stage contributes to a group, in the order LINK-3 numbers them: rank 0 leads.
enum class stage_family : u8
{
    raytracing,
    graphics,
    compute,
};

[[nodiscard]] stage_family kind_of(check::stage s)
{
    if (s >= check::stage::raygen)
        return stage_family::raytracing;
    return s == check::stage::compute ? stage_family::compute : stage_family::graphics;
}

/// LINK-3: raygens, misses, hit groups' stages, callables; a graphics pipeline's stages in pipeline order.
[[nodiscard]] i32 rank_of(check::stage s)
{
    switch (s)
    {
    case check::stage::raygen:
    case check::stage::task:
    case check::stage::compute:
        return 0;
    case check::stage::miss:
    case check::stage::mesh:
        return 1;
    case check::stage::closest_hit:
    case check::stage::any_hit:
    case check::stage::intersection:
    case check::stage::vertex:
        return 2;
    case check::stage::callable:
    case check::stage::tessellation_control:
        return 3;
    case check::stage::tessellation_evaluation:
        return 4;
    case check::stage::geometry:
        return 5;
    case check::stage::pixel:
        return 6;
    case check::stage::none:
        break;
    }
    return 7;
}

/// LINK-2: a module's symbol by its module, a program file's own by its library path, the prelude's bare.
[[nodiscard]] cc::string key_of(check::checked_module const& m, cc::string_view source_name, check::symbol_id id)
{
    auto const& s = m.at(id);
    if (s.file < m.prelude_file_count())
        return cc::string(s.name);
    if (s.file < i32(m.file_modules.size()) && !m.file_modules[s.file].empty())
        return cc::format("{}.{}", m.file_modules[s.file], s.name);
    return cc::format("{}:{}", source_name, s.name);
}

/// The callables tables `legal` calls, in the order of its file's callable section.
[[nodiscard]] cc::vector<check::symbol_id> tables_called(check::checked_module const& m,
                                                         check::flat_entry_point const& legal)
{
    auto result = cc::vector<check::symbol_id>();
    for (auto const t : m.callable_tables)
        for (auto const& x : legal.exprs)
            if (auto const* const base = x.node.try_as<check::flat_callable_base>(); base != nullptr && base->table == t)
            {
                result.push_back(t);
                break;
            }
    return result;
}

/// The ray sets of the program and the modules it reaches that hold a ray type of payload `type`.
[[nodiscard]] cc::vector<check::symbol_id> ray_sets_holding(driver::impl::front_end const& front, check::type_id type)
{
    auto const& m = front.module;
    auto result = cc::vector<check::symbol_id>();
    for (auto i = isize(0); i < m.symbols.size(); ++i)
    {
        auto const& s = m.symbols[i];
        if (s.kind != check::symbol_kind::structure || s.state != check::symbol_state::checked
            || !m.is_reached_file(s.file) || !ast::is_valid(s.declaration) || !check::is_valid(s.type))
            continue;
        auto const* const d = front.asts[s.file]->at(s.declaration).node.try_as<ast::struct_decl>();
        if (d == nullptr || !d->is_ray_set)
            continue;
        for (auto const& ray : m.at(m.at(s.type).members))
            if (ray.type == type)
            {
                result.push_back(check::symbol_id(i));
                break;
            }
    }
    return result;
}

[[nodiscard]] bool same_options(cc::span<check::option_value const> a, cc::span<check::option_value const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (!(a[i] == b[i]))
            return false;
    return true;
}

/// FNV-1a, 64 bits: what `numbering_hash` keys a cache on, the same in every process.
struct stable_hash
{
    u64 value = 0xcbf29ce484222325ull;

    void add(cc::string_view text)
    {
        for (auto const c : text)
        {
            value ^= u64(u8(c));
            value *= 0x100000001b3ull;
        }
    }
};

/// `text` as a JSON string, quotes included.
[[nodiscard]] cc::string json_string(cc::string_view text)
{
    auto result = cc::string("\"");
    for (auto const c : text)
    {
        if (c == '"' || c == '\\')
        {
            result += '\\';
            result += c;
        }
        else if (c == '\n')
            result += "\\n";
        else if (c == '\t')
            result += "\\t";
        else if (u8(c) < 0x20)
            result += cc::format("\\u{:04x}", u32(u8(c)));
        else
            result += c;
    }
    result += '"';
    return result;
}

[[nodiscard]] cc::string json_strings(cc::span<cc::string const> texts)
{
    auto result = cc::string("[");
    for (auto i = isize(0); i < texts.size(); ++i)
        result += cc::format("{}{}", i == 0 ? "" : ", ", json_string(texts[i]));
    result += "]";
    return result;
}
} // namespace

struct sgl::link_result::state
{
    link_map map;
    cc::vector<emit::target> targets;
    check::feature_set device_features;
    check::float_level float_floor = check::float_level::fast;
    cc::vector<cc::unique_ptr<linked_file>> files;
    cc::vector<linked_entry> entries;
};

sgl::link_result::link_result(cc::unique_ptr<state> s) : _state(cc::move(s))
{
}
sgl::link_result::link_result(link_result&& rhs) noexcept = default;
sgl::link_result& sgl::link_result::operator=(link_result&& rhs) noexcept = default;
sgl::link_result::~link_result() = default;

sgl::link_map const& sgl::link_result::map() const
{
    return _state->map;
}

sgl::isize sgl::link_result::entry_count() const
{
    return _state->entries.size();
}

cc::string_view sgl::link_result::entry_name(isize entry) const
{
    CC_ASSERT(entry >= 0 && entry < _state->entries.size(), "entry must be a position in the request's entries");
    return _state->entries[entry].name;
}

sgl::u64 sgl::link_result::numbering_hash(isize entry) const
{
    CC_ASSERT(entry >= 0 && entry < _state->entries.size(), "entry must be a position in the request's entries");
    return _state->entries[entry].hash;
}

cc::result<sgl::emitted_source, cc::string> sgl::link_result::emit(isize entry,
                                                                   emit::target t,
                                                                   link_emit_options const& options) const
{
    auto const& s = *_state;
    if (entry < 0 || entry >= s.entries.size())
        return cc::error(cc::format("{}: error: no entry {} (the group lists {})\n", s.map.name, entry, s.entries.size()));
    auto target = isize(-1);
    for (auto i = isize(0); i < s.targets.size(); ++i)
        if (s.targets[i] == t)
            target = i;
    if (target < 0)
        return cc::error(cc::format("{}: error: the group was linked for no target {}\n", s.map.name, emit::to_string(t)));
    auto const& e = s.entries[entry];
    auto const& file = *s.files[e.file];
    auto const& front = file.written[target];
    auto const* found = static_cast<check::flat_entry_point const*>(nullptr);
    for (auto const& candidate : front.module.entry_points)
        if (candidate.name == e.entry_point && candidate.entry_stage == e.stage)
            found = &candidate;
    // the check that numbered held it, and a check for a target holds what it does, its own errors aside (CHK-426)
    if (found == nullptr)
        return cc::error(cc::format("{}: error: no {} entry point named '{}' for {}\n", file.source_name,
                                    check::stage_name(e.stage), e.entry_point, emit::to_string(t)));
    if (auto own = front.errors_of(found->function); !own.empty())
        return cc::error(cc::move(own));
    auto result = driver::impl::emit_text(
        front, *found, t, options.line_directives, options.diagnostics, options.guards_undefined, options.software_trace,
        options.weakens_float, s.float_floor, s.device_features, options.fills, options.core, options.native,
        options.native_debug_info, emit::mesh_form::native, nullptr, nullptr, nullptr, &file.numberings[target]);
    driver::impl::add_source_facts(result, front, *found);
    driver::impl::finish_warnings(result, file.source_name, *found);
    return result;
}

namespace
{
/// The group's numbering as LINK-3 builds it, symbol by key.
struct numberer
{
    struct numbered_sampler
    {
        cc::string key;
        sgl::described_file_sampler settings;
    };
    struct numbered_table
    {
        cc::string key;
        i32 first_record = 0;
        i32 records = 0;
        cc::vector<cc::string> callables;
        bool has_host = false;
    };
    cc::vector<numbered_sampler> samplers;
    cc::vector<numbered_table> tables;
    i32 records = 0;

    [[nodiscard]] i32 slot_of(cc::string_view key) const
    {
        for (auto i = isize(0); i < samplers.size(); ++i)
            if (samplers[i].key == key)
                return i32(i);
        return -1;
    }

    [[nodiscard]] i32 record_of(cc::string_view key) const
    {
        for (auto const& t : tables)
            if (t.key == key)
                return t.first_record;
        return -1;
    }

    void sampler(check::checked_module const& m, cc::string_view source_name, check::symbol_id id)
    {
        auto key = key_of(m, source_name, id);
        if (slot_of(key) >= 0)
            return;
        auto settings = driver::impl::describe_file_sampler(m, id);
        settings.name = key;
        settings.index = i32(samplers.size());
        samplers.push_back({.key = cc::move(key), .settings = cc::move(settings)});
    }

    void table(check::checked_module const& m, cc::string_view source_name, check::symbol_id id)
    {
        auto key = key_of(m, source_name, id);
        if (record_of(key) >= 0)
            return;
        auto const& p = m.pipelines[m.at(id).info];
        auto t = numbered_table{.key = cc::move(key),
                                .first_record = records,
                                .records = i32(p.records.count),
                                .has_host = p.has_host_callables};
        for (auto const callable : m.at(p.records))
            t.callables.push_back(key_of(m, source_name, callable));
        records += t.records;
        tables.push_back(cc::move(t));
    }
};

/// A link refusal, `<group>: error: <kind>: <detail>`.
[[nodiscard]] cc::string refusal(cc::string_view group, cc::string_view kind, cc::string_view detail)
{
    return cc::format("{}: error: {}: {}\n", group, kind, detail);
}
} // namespace

cc::result<sgl::link_result, cc::string> sgl::link(link_request const& request)
{
    auto const group = request.name;
    if (request.entries.empty())
        return cc::error(refusal(group, "link-empty", "the group lists no entry"));
    auto s = cc::make_unique<link_result::state>();
    s->map.name = cc::string(group);
    s->targets = cc::vector<emit::target>::create_copy_of(request.targets);
    s->device_features = request.device_features;
    s->float_floor = request.float_floor;

    // LINK-1: each file once per set of options its entries are built with
    for (auto const& asked : request.entries)
    {
        auto file = i32(-1);
        for (auto i = isize(0); i < s->files.size(); ++i)
            // as the library finds it, so two spellings of one path are one file and name its own symbols alike
            if (is_same_path(s->files[i]->source_name, asked.source_name)
                && same_options(s->files[i]->options, asked.options))
                file = i32(i);
        if (file < 0)
        {
            auto const* source = static_cast<library_file const*>(nullptr);
            for (auto const& f : request.library)
                if (is_same_path(f.name, asked.source_name))
                    source = &f;
            if (source == nullptr)
                return cc::error(refusal(group, "link-no-entry-point",
                                         cc::format("'{}' names no file of the library", asked.source_name)));
            auto made = cc::make_unique<linked_file>();
            made->source_name = cc::string(asked.source_name);
            made->options = cc::vector<check::option_value>::create_copy_of(asked.options);
            // the check that numbers is no target's, as `describe`'s, so the map is every target's (LINK-6)
            made->numbered
                = driver::impl::run_front_end(source->source, made->source_name, request.library, made->options, {});
            if (!made->numbered.errors.empty())
                return cc::error(made->numbered.errors);
            for (auto const t : request.targets)
            {
                made->written.push_back(
                    driver::impl::run_front_end(source->source, made->source_name, request.library, made->options,
                                                emit::compile_target_of(t, request.device_features)));
                if (!made->written.back().errors.empty())
                    return cc::error(made->written.back().errors);
            }
            file = i32(s->files.size());
            s->files.push_back(cc::move(made));
        }
        auto name = cc::format("{}:{}", asked.source_name, asked.entry_point);
        for (auto const& o : asked.options)
            name += cc::format(":{}={}", o.name, o.value);
        s->entries.push_back({.file = file, .entry_point = cc::string(asked.entry_point), .name = cc::move(name)});
    }

    // Each entry's tree as the check that numbers flattened it, legalized as `describe` legalizes it.
    struct found_entry
    {
        check::flat_entry_point const* e = nullptr;
        check::flat_entry_point legal;
    };
    auto found = cc::vector<found_entry>();
    for (auto& e : s->entries)
    {
        auto const& file = *s->files[e.file];
        auto const& m = file.numbered.module;
        auto const& asked = request.entries[found.size()];
        auto const* entry = static_cast<check::flat_entry_point const*>(nullptr);
        for (auto const& candidate : m.entry_points)
            if (candidate.name == asked.entry_point)
                entry = &candidate;
        if (entry == nullptr)
        {
            // CHK-403: one that expects a diagnostic is judged by it, and is no missing entry point
            if (file.numbered.expectation_of(asked.entry_point) != nullptr)
                return cc::error(refusal(group, "link-no-entry-point",
                                         cc::format("'{}' of {} expects a diagnostic, so it is never built",
                                                    asked.entry_point, file.source_name)));
            // CHK-402: an entry point whose own body did not check has no flat tree, and its errors say why
            for (auto i = isize(0); i < m.symbols.size(); ++i)
                if (auto const& sym = m.symbols[i]; sym.kind == check::symbol_kind::function
                                                    && sym.name == asked.entry_point
                                                    && sym.file == file.numbered.program_file())
                    if (auto own = file.numbered.errors_of(check::symbol_id(i)); !own.empty())
                        return cc::error(cc::move(own));
            auto held = cc::string();
            for (auto const& c : m.entry_points)
                held.appendf("{}{} '{}'", held.empty() ? "" : ", ", check::stage_name(c.entry_stage), c.name);
            return cc::error(
                refusal(group, "link-no-entry-point",
                        cc::format("{} holds no entry point named '{}' (it holds: {})", file.source_name,
                                   asked.entry_point, held.empty() ? cc::string_view("none") : cc::string_view(held))));
        }
        if (asked.stage != check::stage::none && entry->entry_stage != asked.stage)
            return cc::error(refusal(
                group, "link-no-entry-point",
                cc::format("'{}' of {} is a {} entry point, and a {} one was listed", asked.entry_point,
                           file.source_name, check::stage_name(entry->entry_stage), check::stage_name(asked.stage))));
        if (auto own = file.numbered.errors_of(entry->function); !own.empty())
            return cc::error(cc::move(own));
        e.stage = entry->entry_stage;
        found.push_back(
            {.e = entry, .legal = entry->legalized != nullptr ? *entry->legalized : check::legalize(m, *entry)});
    }

    // LINK-1: one pipeline's stages; a compute entry point is a group of one
    auto const first_kind = kind_of(s->entries[0].stage);
    for (auto const& e : s->entries)
        if (kind_of(e.stage) != first_kind || (first_kind == stage_family::compute && s->entries.size() > 1))
            return cc::error(refusal(group, "link-mixed-stages",
                                     cc::format("'{}' is a {} entry point, and '{}' a {} one: a group is one "
                                                "pipeline's "
                                                "stages, and a compute entry point a group of its own",
                                                s->entries[0].name, check::stage_name(s->entries[0].stage), e.name,
                                                check::stage_name(e.stage))));

    // LINK-3: by rank, each rank in listing order; the leading rank's files keep their whole tables
    auto order = cc::vector<isize>();
    auto leading = 7;
    for (auto const& e : s->entries)
        leading = rank_of(e.stage) < leading ? rank_of(e.stage) : leading;
    // a ray-tracing group without a raygen has no stage that keeps its file's numbering
    if (first_kind == stage_family::raytracing)
        leading = 0;
    for (auto rank = 0; rank <= 7; ++rank)
        for (auto i = isize(0); i < s->entries.size(); ++i)
            if (rank_of(s->entries[i].stage) == rank)
                order.push_back(i);
    auto n = numberer();
    for (auto const i : order)
    {
        auto const& e = s->entries[i];
        auto const& file = *s->files[e.file];
        auto const& m = file.numbered.module;
        auto const& legal = found[i].legal;
        if (rank_of(e.stage) == leading)
        {
            // EMIT-133's whole table and CHK-562's whole section, so the first such file numbers as compiled alone
            for (auto k = isize(0); k < m.symbols.size(); ++k)
                if (m.symbols[k].kind == check::symbol_kind::sampler && m.is_reached_file(m.symbols[k].file))
                    n.sampler(m, file.source_name, check::symbol_id(k));
            for (auto const t : m.callable_tables)
                n.table(m, file.source_name, t);
            continue;
        }
        for (auto const id : driver::impl::file_samplers_of(legal))
            n.sampler(m, file.source_name, id);
        for (auto const t : tables_called(m, legal))
            n.table(m, file.source_name, t);
        // a listed callable's own table, so the map gives it a record whether or not an entry calls it
        if (e.stage == check::stage::callable)
            for (auto const& p : m.pipelines)
            {
                if (p.kind != check::pipeline_kind::callables || !m.is_reached_file(m.at(p.symbol).file))
                    continue;
                for (auto const callable : m.at(p.records))
                    if (callable == found[i].e->function)
                    {
                        n.table(m, file.source_name, p.symbol);
                        break;
                    }
            }
    }

    // LINK-9: a slot past what a stage holds, which an entry reaches
    for (auto i = isize(0); i < s->entries.size(); ++i)
    {
        auto const& file = *s->files[s->entries[i].file];
        for (auto const id : driver::impl::file_samplers_of(found[i].legal))
            if (auto const slot = n.slot_of(key_of(file.numbered.module, file.source_name, id));
                slot >= emit::impl::k_max_file_samplers)
                return cc::error(refusal(group, "too-many-samplers",
                                         cc::format("group '{}' gives sampler '{}' slot {}, which '{}' reaches, and a "
                                                    "stage holds {}",
                                                    group, key_of(file.numbered.module, file.source_name, id), slot,
                                                    s->entries[i].name, emit::impl::k_max_file_samplers)));
    }
    // LINK-9: the host's callables follow the group's last record (CHK-343)
    for (auto i = isize(0); i + 1 < n.tables.size(); ++i)
        if (n.tables[i].has_host)
            return cc::error(refusal(group, "link-host-callables",
                                     cc::format("table '{}' takes .host, and table '{}' follows it in the group's "
                                                "callable section, where the host's callables would stand",
                                                n.tables[i].key, n.tables[i + 1].key)));

    // LINK-8: one ray set for every entry that traces, misses or hits
    struct ray_set_source
    {
        cc::string key;
        isize entry = -1;
        check::symbol_id symbol = check::symbol_id::none;
    };
    auto candidates = cc::vector<ray_set_source>();
    auto is_constrained = false;
    auto constrained_by = cc::string();
    for (auto const i : order)
    {
        auto const& file = *s->files[s->entries[i].file];
        auto const& m = file.numbered.module;
        auto const& e = *found[i].e;
        auto own = cc::vector<ray_set_source>();
        auto const add = [&](cc::vector<ray_set_source>& to, check::symbol_id id)
        {
            auto key = key_of(m, file.source_name, id);
            for (auto const& known : to)
                if (known.key == key)
                    return;
            to.push_back({.key = cc::move(key), .entry = i, .symbol = id});
        };
        for (auto const& traced : e.traced_rays)
            add(own, traced.set);
        if (own.size() > 1)
            return cc::error(
                refusal(group, "link-ray-set-mismatch",
                        cc::format("'{}' traces ray sets '{}' and '{}'", s->entries[i].name, own[0].key, own[1].key)));
        auto const takes_payload = (e.entry_stage == check::stage::miss || e.entry_stage == check::stage::closest_hit
                                    || e.entry_stage == check::stage::any_hit)
                                && check::is_valid(e.input);
        if (takes_payload)
        {
            auto held = cc::vector<ray_set_source>();
            for (auto const id : ray_sets_holding(file.numbered, e.input))
                add(held, id);
            if (own.empty())
                own = cc::move(held);
            else if (!held.values().any([&](ray_set_source const& h) { return h.key == own[0].key; }))
                return cc::error(refusal(group, "link-ray-set-mismatch",
                                         cc::format("'{}' traces ray set '{}', and takes a payload of none of its rays",
                                                    s->entries[i].name, own[0].key)));
        }
        if (own.empty())
            continue;
        if (!is_constrained)
        {
            candidates = cc::move(own);
            is_constrained = true;
            constrained_by = s->entries[i].name;
            continue;
        }
        auto kept = cc::vector<ray_set_source>();
        for (auto& c : candidates)
            if (own.values().any([&](ray_set_source const& o) { return o.key == c.key; }))
                kept.push_back(cc::move(c));
        if (kept.empty())
            return cc::error(refusal(
                group, "link-ray-set-mismatch",
                cc::format("'{}' uses ray set '{}', and '{}' another", s->entries[i].name, own[0].key, constrained_by)));
        candidates = cc::move(kept);
    }
    if (!candidates.empty())
    {
        // several sets every entry agrees with: the first by name, which no listing order changes
        auto best = isize(0);
        for (auto i = isize(1); i < candidates.size(); ++i)
            if (candidates[i].key < candidates[best].key)
                best = i;
        auto const& chosen = candidates[best];
        auto const& m = s->files[s->entries[chosen.entry].file]->numbered.module;
        s->map.ray_set = chosen.key;
        auto const rays = m.at(m.at(m.at(chosen.symbol).type).members);
        s->map.multiplier = i32(rays.size());
        for (auto r = isize(0); r < rays.size(); ++r)
            s->map.rays.push_back({.name = cc::string(rays[r].name), .contribution = i32(r), .miss = i32(r)});
    }

    // the map, each entry's hash and whether its numbers moved, and each check's numbering of its own symbols
    for (auto& sampler : n.samplers)
        s->map.samplers.push_back(
            {.symbol = sampler.key, .slot = sampler.settings.index, .settings = cc::move(sampler.settings)});
    for (auto& t : n.tables)
    {
        s->map.has_host_callables = s->map.has_host_callables || t.has_host;
        s->map.tables.push_back(
            {.symbol = t.key, .first_record = t.first_record, .callables = cc::move(t.callables), .has_host = t.has_host});
    }
    for (auto i = isize(0); i < s->entries.size(); ++i)
    {
        auto& e = s->entries[i];
        auto const& file = *s->files[e.file];
        auto const& m = file.numbered.module;
        // LINK-7: what the entry reaches in the tree that numbered, and in each target's own, which may take a branch
        // of `target_api` or `has` the first does not (CHK-426); each once, in that order
        auto hash = stable_hash();
        auto is_renumbered = false;
        auto hashed = cc::vector<cc::string>();
        auto const reach = [&](check::checked_module const& tree, check::flat_entry_point const& legal)
        {
            for (auto const id : driver::impl::file_samplers_of(legal))
            {
                auto key = key_of(tree, file.source_name, id);
                auto const slot = n.slot_of(key);
                auto item = cc::format("s{}={};", key, slot);
                if (hashed.values().contains(item))
                    continue;
                hash.add(item);
                hashed.push_back(cc::move(item));
                is_renumbered = is_renumbered || slot != emit::impl::file_sampler_index(tree, id);
            }
            for (auto const t : tables_called(tree, legal))
            {
                auto key = key_of(tree, file.source_name, t);
                auto const record = n.record_of(key);
                auto item = cc::format("t{}={};", key, record);
                if (hashed.values().contains(item))
                    continue;
                hash.add(item);
                hashed.push_back(cc::move(item));
                is_renumbered = is_renumbered || record != tree.callable_offset_of(t);
            }
        };
        reach(m, found[i].legal);
        for (auto const& front : file.written)
            for (auto const& candidate : front.module.entry_points)
                if (candidate.name == e.entry_point && candidate.entry_stage == e.stage)
                    reach(front.module, candidate.legalized != nullptr ? *candidate.legalized
                                                                       : check::legalize(front.module, candidate));
        e.hash = hash.value;
        // an entry listed twice, as one closest hit in two hit groups, is one build
        if (is_renumbered && !s->map.renumbered.values().contains(e.name))
            s->map.renumbered.push_back(e.name);
    }
    for (auto& file : s->files)
        for (auto const& front : file->written)
        {
            auto const& m = front.module;
            auto numbering = emit::numbering();
            for (auto k = isize(0); k < m.symbols.size(); ++k)
            {
                auto const id = check::symbol_id(k);
                auto const& sym = m.symbols[k];
                if (sym.kind == check::symbol_kind::sampler)
                {
                    auto key = key_of(m, file->source_name, id);
                    if (auto const slot = n.slot_of(key); slot >= 0)
                        numbering.samplers.push_back({.symbol = id, .value = slot, .name = cc::move(key)});
                }
                else if (sym.kind == check::symbol_kind::pipeline && sym.state == check::symbol_state::checked
                         && sym.info >= 0 && sym.info < m.pipelines.size()
                         && m.pipelines[sym.info].kind == check::pipeline_kind::callables
                         && m.pipelines[sym.info].symbol == id)
                {
                    auto key = key_of(m, file->source_name, id);
                    if (auto const record = n.record_of(key); record >= 0)
                        numbering.tables.push_back({.symbol = id, .value = record, .name = cc::move(key)});
                }
            }
            file->numberings.push_back(cc::move(numbering));
        }
    return link_result(cc::move(s));
}

cc::string sgl::to_json(link_map const& map)
{
    auto out = cc::string();
    out += cc::format("{{\n  \"name\": {},\n  \"samplers\": [", json_string(map.name));
    for (auto i = isize(0); i < map.samplers.size(); ++i)
    {
        auto const& s = map.samplers[i];
        auto const& st = s.settings.settings;
        out += cc::format("{}\n    {{\"symbol\": {}, \"slot\": {}, \"sampler_type\": {}, \"shape\": {}, \"settings\": "
                          "{{\"min_filter\": {}, \"mag_filter\": {}, \"mip_filter\": {}, \"address_u\": {}, "
                          "\"address_v\": {}, \"address_w\": {}, \"compare\": {}, \"max_anisotropy\": {}, "
                          "\"min_lod\": {}, \"max_lod\": {}, \"mip_lod_bias\": {}}}}}",
                          i == 0 ? "" : ",", json_string(s.symbol), s.slot, json_string(s.settings.sampler_type),
                          json_string(s.settings.shape), json_string(st.min_filter), json_string(st.mag_filter),
                          json_string(st.mip_filter), json_string(st.address_u), json_string(st.address_v),
                          json_string(st.address_w), json_string(st.compare), st.max_anisotropy, st.min_lod, st.max_lod,
                          st.mip_lod_bias);
    }
    out += map.samplers.empty() ? "],\n  \"tables\": [" : "\n  ],\n  \"tables\": [";
    for (auto i = isize(0); i < map.tables.size(); ++i)
    {
        auto const& t = map.tables[i];
        out += cc::format("{}\n    {{\"symbol\": {}, \"first_record\": {}, \"callables\": {}, \"has_host\": {}}}",
                          i == 0 ? "" : ",", json_string(t.symbol), t.first_record, json_strings(t.callables),
                          t.has_host ? "true" : "false");
    }
    out += map.tables.empty() ? "],\n" : "\n  ],\n";
    out += cc::format("  \"has_host_callables\": {},\n  \"ray_set\": {},\n  \"multiplier\": {},\n  \"rays\": [",
                      map.has_host_callables ? "true" : "false", json_string(map.ray_set), map.multiplier);
    for (auto i = isize(0); i < map.rays.size(); ++i)
        out += cc::format("{}{{\"name\": {}, \"contribution\": {}, \"miss\": {}}}", i == 0 ? "" : ", ",
                          json_string(map.rays[i].name), map.rays[i].contribution, map.rays[i].miss);
    out += cc::format("],\n  \"renumbered\": {}\n}}\n", json_strings(map.renumbered));
    return out;
}
