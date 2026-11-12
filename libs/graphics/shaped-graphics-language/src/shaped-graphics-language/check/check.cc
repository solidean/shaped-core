#include "check.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/impl/relocate.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// The files of one check in its order, and how they fall into modules.
struct planned_files
{
    cc::vector<module_file> files;
    module_plan plan;
};

planned_files plan_files(cc::span<module_file const> prelude, cc::span<module_file const> library, module_file program)
{
    auto result = planned_files{.plan = plan_modules(library, program)};
    result.files.push_back_range(prelude);
    for (auto const i : result.plan.library_order)
        result.files.push_back(library[i]);
    result.files.push_back(program);
    return result;
}

/// Hands `c` the modules of `plan`, with `prelude_count` files of the prelude in front of the plan's, and reports what
/// the plan found wrong.
void apply_plan(checker& c, module_plan& plan, i32 prelude_count)
{
    c.prelude_count = prelude_count;
    c.out.prelude_files = prelude_count;
    c.out.library_files = plan.library_order;
    c.out.reached_library_files = plan.library_order;
    c.out.file_modules = {};
    for (auto i = i32(0); i < prelude_count; ++i)
        c.out.file_modules.push_back({});
    for (auto const module : plan.file_module)
        c.out.file_modules.push_back(plan.modules[module]);
    c.modules = cc::move(plan.modules);
    c.file_module = cc::vector<i32>::create_filled(prelude_count, -1);
    c.file_module.push_back_range(plan.file_module);
    c.uses = {};
    for (auto i = i32(0); i < prelude_count; ++i)
        c.uses.push_back({});
    for (auto& u : plan.uses)
        c.uses.push_back(cc::move(u));
    for (auto& d : plan.diagnostics)
    {
        d.file += prelude_count;
        for (auto& n : d.notes)
            n.file += prelude_count;
        c.out.diagnostics.push_back(cc::move(d));
    }
}
} // namespace

checked_module sgl::check::check(cc::span<module_file const> prelude,
                                 cc::span<module_file const> library,
                                 module_file program,
                                 builtins::registry const& builtins,
                                 cc::span<option_value const> options,
                                 compile_target const& target)
{
    auto planned = plan_files(prelude, library, program);
    auto c = checker{.files = planned.files, .builtins = builtins};
    c.out.builtins = &builtins;
    apply_plan(c, planned.plan, i32(prelude.size()));
    c.options = options;
    c.compiled_target = target;
    c.run();
    return cc::move(c.out);
}

checked_module sgl::check::check(cc::span<module_file const> prelude,
                                 module_file user,
                                 builtins::registry const& builtins,
                                 cc::span<option_value const> options,
                                 compile_target const& target)
{
    return check(prelude, {}, user, builtins, options, target);
}

checked_module sgl::check::check(cc::span<module_file const> prelude,
                                 module_file user,
                                 cc::span<option_value const> options,
                                 compile_target const& target)
{
    return check(prelude, user, builtins::default_registry(), options, target);
}

sgl::check::checked_prelude::checked_prelude() = default;
sgl::check::checked_prelude::checked_prelude(checked_prelude&&) noexcept = default;
sgl::check::checked_prelude& sgl::check::checked_prelude::operator=(checked_prelude&&) noexcept = default;
sgl::check::checked_prelude::~checked_prelude() = default;

sgl::check::checked_module const& sgl::check::checked_prelude::module() const
{
    return _state->out;
}

namespace
{
/// Ends a layer's check: the empty program behind it goes, its tables are shared from here, and a program's check
/// resumes after its `files` files.
/// Every program's check reads the layer's entries in place rather than copying them.
impl::resume_point freeze_layer(checker& c, i32 files, bool is_link = false)
{
    // The empty file's tables are the program's to replace, and nothing may point into the empty file past here.
    c.files = {};
    c.out.files.remove_back();
    c.file_features.remove_back();
    c.file_module.remove_back();
    c.uses.remove_back();
    c.out.file_modules.remove_back();
    return seal_layer(c, files, is_link);
}
} // namespace

impl::resume_point sgl::check::impl::seal_layer(checker& c, i32 files, bool is_link)
{
    c.open_types_below.clear();
    for (auto i = isize(0); i < c.out.types.size(); ++i)
        if (c.is_open(type_id(i)))
            c.open_types_below.push_back(type_id(i));
    c.open_calls_below.clear();
    for (auto i = isize(0); i < c.out.call_records.size(); ++i)
        if (auto const b = c.out.at(c.out.call_records[i].type_arguments); !b.empty() && !c.is_closed(b))
            c.open_calls_below.push_back(i);
    // The layer's names, once, for every program behind it; a program's check makes a set of its own names alone.
    // A program behind a link takes the names of the files it reaches instead, so a link's are made only where its next
    // link flattens a test (`checker::module_names`).
    c.module_names_below = nullptr;
    c.module_names_below_count = c.out.symbols.size();
    if (!is_link)
        c.module_names_below = c.names_up_to(c.module_names_below_count);
    // CHK-527: the prelude's own entry points mint over every name of the layer, which outlives the sets they were
    // flattened with: those go with `out.module_names` below
    for (auto i = c.out.internal_entry_points.first_own(); i < c.out.internal_entry_points.size(); ++i)
    {
        auto& e = c.out.internal_entry_points.own(i);
        auto const repoint = [&](name_mint& names)
        {
            names.also_taken = c.module_names_below.get();
            names.also_taken_below = nullptr;
        };
        repoint(e.names);
        if (e.legalized != nullptr)
        {
            auto legalized = *e.legalized;
            repoint(legalized.names);
            e.legalized = cc::make_shared<flat_entry_point const>(cc::move(legalized));
        }
    }
    c.out.freeze_tables();
    c.prelude_names.freeze();
    c.prelude_operators.freeze();
    c.operators.freeze();
    c.type_scopes.freeze();
    c.interned_types.freeze();
    c.prelude_visible.freeze();
    c.instances.freeze();
    c.own_module_names = nullptr;
    c.module_names_for = -1;
    c.out.module_names = {};
    return {
        .files = files,
        .symbols = c.out.symbols.size(),
        .extensions = c.pending_extensions.size(),
        .tests = c.out.tests.size(),
        .wide_literals = c.wide_literals.size(),
        .notes = c.notes.size(),
        .calls = c.calls.size(),
        .call_records = c.out.call_records.size(),
        .types = c.out.types.size(),
    };
}

namespace
{
/// Whether `plan` reaches the modules `layer` holds, in the same order and under the same names.
/// The program's own module is not the layer's, and may be named anything.
bool plan_matches(module_plan const& plan, cc::span<i32 const> library_order, cc::span<cc::string const> modules)
{
    if (!may_layer(plan) || plan.library_order.size() != library_order.size() || plan.modules.size() != modules.size())
        return false;
    for (auto i = isize(0); i < library_order.size(); ++i)
        if (plan.library_order[i] != library_order[i])
            return false;
    for (auto i = isize(1); i < modules.size(); ++i)
        if (plan.modules[i] != modules[i])
            return false;
    return true;
}
} // namespace

bool sgl::check::impl::may_layer(module_plan const& plan)
{
    if (!plan.diagnostics.empty() || plan.library_order.empty())
        return false;
    for (auto i = isize(0); i < plan.library_order.size(); ++i)
        if (plan.file_module[i] == 0)
            return false;
    return true;
}

cc::optional<checked_prelude> sgl::check::check_prelude(cc::span<module_file const> prelude,
                                                        builtins::registry const& builtins)
{
    // The program's file stands behind the prelude even when it is empty, since that is what makes a file the prelude's.
    auto const empty_file = parse("");
    auto const empty_ast = ast::build(empty_file);
    auto result = checked_prelude();
    result._files.push_back_range(prelude);
    result._prelude_count = i32(prelude.size());
    auto files = cc::vector<module_file>();
    files.push_back_range(prelude);
    files.push_back({.file = empty_file, .ast = empty_ast});

    result._state = cc::make_unique<checker>(checker{.files = files, .builtins = builtins});
    auto& c = *result._state;
    c.out.builtins = &builtins;
    auto plan = plan_modules({}, files.back());
    apply_plan(c, plan, i32(prelude.size()));
    c.run();
    if (!c.out.diagnostics.empty())
        return {};
    result._resume = freeze_layer(c, i32(prelude.size()));
    return result;
}

cc::optional<checked_prelude> sgl::check::check_modules(checked_prelude const& prelude,
                                                        cc::span<module_file const> library,
                                                        module_file program)
{
    // a layer is made over the prelude alone, never over another one
    CC_ASSERT(prelude._library_order.empty(), "check_modules continues from the prelude's layer");
    auto plan = plan_modules(library, program);
    if (!may_layer(plan))
        return {};

    // The modules behind an empty program: the program's own `use` lines name nothing of the empty file.
    auto const empty_file = parse("");
    auto const empty_ast = ast::build(empty_file);
    auto result = checked_prelude();
    result._files.push_back_range(cc::span<module_file const>(prelude._files));
    result._prelude_count = prelude._prelude_count;
    for (auto const i : plan.library_order)
        result._files.push_back(library[i]);
    result._library_order = plan.library_order;
    result._modules = plan.modules;
    auto files = result._files;
    files.push_back({.file = empty_file, .ast = empty_ast});
    plan.uses.back() = {};

    result._state = cc::make_unique<checker>(*prelude._state);
    auto& c = *result._state;
    c.files = files;
    apply_plan(c, plan, prelude._prelude_count);
    c.run(prelude._resume);

    // An unused `require` is judged after the program's entry points are flattened, which the program's check does.
    auto kept = cc::vector<located_diagnostic>();
    for (auto& d : c.out.diagnostics)
        if (d.what.kind != diagnostic_kind::unused_require)
            kept.push_back(cc::move(d));
    if (!kept.empty())
        return {};
    c.out.diagnostics = {};
    result._resume = freeze_layer(c, i32(result._files.size()));
    return result;
}

bool sgl::check::reaches_modules_to_layer(cc::span<module_file const> library, module_file program)
{
    return may_layer(plan_modules(library, program));
}

bool sgl::check::reaches_the_same_modules(checked_prelude const& layer,
                                          cc::span<module_file const> library,
                                          module_file program)
{
    return !layer._library_order.empty()
        && plan_matches(plan_modules(library, program), layer._library_order, layer._modules);
}

namespace
{
/// A program that uses each of `names` once, by its own name and in order.
cc::string uses_of(cc::span<cc::string const> names)
{
    // each under a name of its own, since two modules' names may end alike, `a.scene` and `b.scene`
    auto text = cc::string();
    for (auto i = isize(0); i < names.size(); ++i)
        text.appendf("use {} as linked_module_{}\n", names[i], i);
    return text;
}

bool contains(cc::span<cc::string const> names, cc::string_view name)
{
    for (auto const& n : names)
        if (n == name)
            return true;
    return false;
}
} // namespace

cc::vector<chained_module> sgl::check::library_chain(cc::span<module_file const> library, cc::span<i32 const> late_files)
{
    // the modules the library declares, each once, in the library's order
    auto named = cc::vector<cc::string>();
    for (auto const& f : library)
    {
        // a parse error checks as no diagnostic, and only a program reaching the file may report it
        if (!f.file.diagnostics.empty() || !f.ast.diagnostics.empty())
            return {};
        if (auto name = impl::module_name_of(f); !name.empty() && !contains(named, name))
            named.push_back(cc::move(name));
    }
    if (named.empty())
        return {};
    // the modules a file of which declares an option
    auto with_options = cc::vector<cc::string>();
    for (auto const& f : library)
    {
        auto const module = impl::module_name_of(f);
        auto has_option = false;
        for (auto const id : f.ast.at(f.ast.declarations))
        {
            auto const& d = f.ast.at(id);
            if (d.node.is<ast::const_decl>())
                for (auto const& a : f.ast.at(d.attributes))
                    has_option = has_option || f.file.text_of(a.name) == "option";
        }
        if (has_option && !module.empty() && !contains(with_options, module))
            with_options.push_back(cc::string(module));
    }
    auto const every_file = parse(uses_of(named));
    auto const every_ast = ast::build(every_file);
    auto const plan = plan_modules(library, {.file = every_file, .ast = every_ast});
    if (!may_layer(plan))
        return {};

    // what each module uses, by its index in the plan
    auto const count = plan.modules.size();
    auto uses = cc::vector<cc::vector<i32>>();
    for (auto m = isize(0); m < count; ++m)
        uses.push_back({});
    for (auto i = isize(0); i < plan.library_order.size(); ++i)
        for (auto const& u : plan.uses[i])
            if (u.module != plan.file_module[i])
                uses[plan.file_module[i]].push_back(u.module);
    // how many modules reach each one, through any number of uses
    auto dependents = cc::vector<i32>::create_filled(count, 0);
    for (auto from = isize(1); from < count; ++from)
    {
        auto seen = cc::vector<u8>::create_filled(count, 0);
        auto pending = uses[from];
        while (!pending.empty())
        {
            auto const m = pending.back();
            pending.remove_back();
            if (seen[m] != 0)
                continue;
            seen[m] = 1;
            ++dependents[m];
            pending.push_back_range(cc::span<i32 const>(uses[m]));
        }
    }
    // Every module after the ones it uses; of those that may come next, the one more modules reach first, so a module
    // nothing uses comes last and an edit of it checks it alone.
    // the modules of the files being edited
    auto is_late = cc::vector<u8>::create_filled(count, 0);
    for (auto i = isize(0); i < plan.library_order.size(); ++i)
        for (auto const f : late_files)
            if (plan.library_order[i] == f)
                is_late[plan.file_module[i]] = 1;
    auto order = cc::vector<chained_module>();
    auto placed = cc::vector<u8>::create_filled(count, 0);
    for (auto step = isize(1); step < count; ++step)
    {
        auto best = -1;
        for (auto m = i32(1); m < i32(count); ++m)
        {
            if (placed[m] != 0)
                continue;
            auto is_ready = true;
            for (auto const u : uses[m])
                is_ready = is_ready && placed[u] != 0;
            if (!is_ready)
                continue;
            if (best < 0)
            {
                best = m;
                continue;
            }
            auto const late_m = is_late[m] != 0;
            auto const late_best = is_late[best] != 0;
            auto const options_m = contains(with_options, plan.modules[m]);
            auto const options_best = contains(with_options, plan.modules[best]);
            if (late_m != late_best                 ? !late_m
                : options_m != options_best         ? !options_m
                : dependents[m] != dependents[best] ? dependents[m] > dependents[best]
                                                    : plan.modules[m] < plan.modules[best])
                best = m;
        }
        // the plan reported a loop otherwise, and `may_layer` refused it
        CC_ASSERT(best >= 0, "a library's modules use each other without a loop");
        placed[best] = 1;
        auto& next = order.emplace_back(
            chained_module{.name = plan.modules[best], .has_options = contains(with_options, plan.modules[best])});
        for (auto i = isize(0); i < plan.library_order.size(); ++i)
            if (plan.file_module[i] == best)
                next.files.push_back(plan.library_order[i]);
    }
    return order;
}

cc::vector<option_value> sgl::check::options_of_module(cc::span<option_value const> options, cc::string_view module)
{
    auto result = cc::vector<option_value>();
    for (auto const& o : options)
        if (o.name.size() > module.size() && o.name.starts_with(module) && o.name[module.size()] == '.')
            result.push_back(o);
    return result;
}

cc::optional<impl::layer_maker::next_link> impl::layer_maker::plan_next(checked_prelude const& base,
                                                                        cc::span<module_file const> library,
                                                                        cc::string_view module)
{
    CC_ASSERT(base._library_order.empty() || base._is_library, "a chain continues the prelude or a layer of its own");
    // the modules of the layers before, then this one: a program using them all, in the chain's order
    auto names = cc::vector<cc::string>();
    for (auto i = isize(1); i < base._modules.size(); ++i)
        names.push_back(base._modules[i]);
    if (contains(names, module))
        return {};
    names.push_back(cc::string(module));
    auto const every_file = parse(uses_of(names));
    auto const every_ast = ast::build(every_file);
    auto plan = plan_modules(library, {.file = every_file, .ast = every_ast});
    if (!may_layer(plan))
        return {};
    // A layer of the chain continues the one before unchanged, and adds exactly its module: one whose uses reach a
    // module no layer before holds comes too early in the order.
    auto const before = base._library_order.size();
    if (plan.modules.size() != names.size() + 1 || plan.library_order.size() <= before)
        return {};
    for (auto i = isize(0); i < before; ++i)
        if (plan.library_order[i] != base._library_order[i])
            return {};

    auto result = checked_prelude();
    result._files.push_back_range(cc::span<module_file const>(base._files));
    result._prelude_count = base._prelude_count;
    for (auto i = before; i < plan.library_order.size(); ++i)
        result._files.push_back(library[plan.library_order[i]]);
    result._library_order = plan.library_order;
    result._modules = plan.modules;
    result._is_library = true;
    result._options = base._options;
    result._names_by_reach = cc::make_unique<cc::mutex<cc::map<cc::string, cc::shared_ptr<cc::set<cc::string>>>>>();
    return next_link{.layer = cc::move(result), .plan = cc::move(plan)};
}

cc::optional<checked_prelude> sgl::check::check_next_module(checked_prelude const& base,
                                                            cc::span<module_file const> library,
                                                            cc::string_view module,
                                                            cc::span<option_value const> options)
{
    auto next = impl::layer_maker::plan_next(base, library, module);
    if (!next.has_value())
        return {};
    auto result = cc::move(next.value().layer);
    auto& plan = next.value().plan;
    result._options.push_back_range(options);
    auto const empty_file = parse("");
    auto const empty_ast = ast::build(empty_file);
    auto files = result._files;
    files.push_back({.file = empty_file, .ast = empty_ast});
    plan.uses.back() = {};

    result._state = cc::make_unique<checker>(*base._state);
    auto& c = *result._state;
    c.files = files;
    apply_plan(c, plan, base._prelude_count);
    c.options = options;
    c.records_interface = true;
    c.run(base._resume);
    c.options = {};
    c.records_interface = false;

    // An unused `require` is judged after the program's entry points are flattened, which the program's check does.
    auto kept = cc::vector<located_diagnostic>();
    for (auto& d : c.out.diagnostics)
        if (d.what.kind != diagnostic_kind::unused_require)
            kept.push_back(cc::move(d));
    if (!kept.empty())
        return {};
    c.out.diagnostics = {};
    result._resume = freeze_layer(c, i32(result._files.size()), true);
    c.part_digest = impl::interface_hash(c, c.run_start, impl::sizes_of(c));
    c.chain.push_back({
        .module = cc::string(module),
        .start = c.run_start,
        .interface_end = c.interface_end,
        .end = impl::sizes_of(c),
        .files = i32(result._files.size()),
        .interface_digest = c.interface_digest,
        .part_digest = c.part_digest,
        .sources_digest = impl::layer_maker::sources_digest(result._files, base._files.size()),
    });
    return result;
}

cc::optional<checked_prelude> sgl::check::check_library(checked_prelude const& prelude,
                                                        cc::span<module_file const> library)
{
    auto const order = library_chain(library);
    if (order.empty())
        return {};
    auto layer = cc::optional<checked_prelude>();
    for (auto const& module : order)
    {
        auto next = check_next_module(layer.has_value() ? layer.value() : prelude, library, module.name);
        if (!next.has_value())
            return {};
        layer = cc::move(next);
    }
    return layer;
}

bool sgl::check::may_continue_from(checked_prelude const& layer, cc::span<module_file const> library, module_file program)
{
    if (!layer._is_library)
        return reaches_the_same_modules(layer, library, program);
    // a layer of the chain holds some modules whole, so a program reaching only those may continue from it
    auto const plan = plan_modules(library, program);
    if (!may_layer(plan))
        return false;
    for (auto i = isize(1); i < plan.modules.size(); ++i)
        if (!contains(layer._modules, plan.modules[i]))
            return false;
    return true;
}

bool sgl::check::layer_serves_options(checked_prelude const& layer, cc::span<option_value const> options)
{
    // An option is the const whose value the compile sets, which is how its own constant names it.
    // A module's is set as `module.name`, named here by its file's module: a layer has no program to be foreign to.
    auto given = cc::vector<option_value>();
    auto const& out = layer._state->out;
    for (auto const& o : options)
        for (auto s = isize(0); s < out.symbols.size(); ++s)
        {
            auto const& symbol = out.symbols[s];
            if (symbol.kind != symbol_kind::constant || out.constants[symbol.info].option != symbol_id(s)
                || symbol.file < layer._prelude_count || symbol.file >= i32(out.file_modules.size()))
                continue;
            if (o.name == cc::format("{}.{}", out.file_modules[symbol.file], symbol.name))
            {
                given.push_back(o);
                break;
            }
        }
    // the same values, whatever order each was given in; a name given twice checks beside the library, which reports it
    auto held = layer._options;
    if (given.size() != held.size())
        return false;
    cc::sort_by(given, &option_value::name);
    cc::sort_by(held, &option_value::name);
    for (auto i = isize(0); i < given.size(); ++i)
        if (!(given[i] == held[i]) || (i > 0 && given[i].name == given[i - 1].name))
            return false;
    return true;
}

checked_module sgl::check::check(checked_prelude const& prelude,
                                 cc::span<module_file const> library,
                                 module_file program,
                                 cc::span<option_value const> options,
                                 compile_target const& target)
{
    CC_ASSERT(layer_serves_options(prelude, options), "a compile that sets a layered module's option checks it itself");
    if (prelude._is_library)
    {
        // The layer's modules stay as the layer planned them; the program joins as module 0 and uses them by name.
        auto plan = plan_modules(library, program);
        CC_ASSERT(may_layer(plan), "a program continues from a layer of the whole library only where it may layer");
        auto files = prelude._files;
        files.push_back(program);
        auto c = *prelude._state;
        c.files = files;
        // the layer dropped its empty program's entries, so the program's are appended where they stood
        c.modules[0] = plan.modules[0];
        c.file_module.push_back(0);
        c.out.file_modules.push_back(plan.modules[0]);
        auto program_uses = cc::vector<module_use>();
        for (auto const& u : plan.uses.back())
            for (auto i = isize(1); i < c.modules.size(); ++i)
                if (c.modules[i] == plan.modules[u.module])
                    program_uses.push_back({.name = u.name, .module = i32(i), .where = u.where});
        c.uses.push_back(cc::move(program_uses));
        c.out.reached_library_files = plan.library_order;
        // A name of a module the program does not use is free to take, as it is beside the modules it reaches alone.
        auto key = cc::string();
        for (auto const r : plan.library_order)
            key.appendf("{},", r);
        c.module_names_below = prelude._names_by_reach->lock(
            [&](cc::map<cc::string, cc::shared_ptr<cc::set<cc::string>>>& by_reach)
            {
                if (auto const* const known = by_reach.get_ptr(key); known != nullptr)
                    return *known;
                auto names = cc::make_shared<cc::set<cc::string>>();
                for (auto i = isize(0); i < c.module_names_below_count; ++i)
                {
                    auto const& s = c.out.symbols[i];
                    if (!s.name.empty() && !takes_no_emitted_name(s) && c.out.is_reached_file(s.file))
                        (void)names->insert(s.name);
                }
                by_reach[key] = names;
                return names;
            });
        c.options = options;
        c.compiled_target = target;
        c.records_interface = false;
        c.run(prelude._resume);
        return cc::move(c.out);
    }
    auto planned
        = plan_files(cc::span<module_file const>(prelude._files).subspan({.offset = 0, .size = prelude._prelude_count}),
                     library, program);
    CC_ASSERT(prelude._library_order.empty() || plan_matches(planned.plan, prelude._library_order, prelude._modules),
              "a program continues from a layer of modules only where it reaches exactly those");
    auto c = *prelude._state;
    c.files = planned.files;
    apply_plan(c, planned.plan, prelude._prelude_count);
    c.options = options;
    c.compiled_target = target;
    c.records_interface = false;
    c.run(prelude._resume);
    return cc::move(c.out);
}

checked_module sgl::check::check(checked_prelude const& prelude,
                                 module_file user,
                                 cc::span<option_value const> options,
                                 compile_target const& target)
{
    return check(prelude, {}, user, options, target);
}

cc::string sgl::check::checked_module::qualified_name_of(symbol_id id) const
{
    auto const module = foreign_module_of(id);
    return module.empty() ? cc::string(at(id).name) : cc::format("{}.{}", module, at(id).name);
}

// ---- number literals ------------------------------------------------------------------------------------------------

namespace
{
/// `classify_number` of a number without a suffix.
number_class classify_unsuffixed(cc::string_view text)
{
    auto const is_digit = [](char c) { return c >= '0' && c <= '9'; };
    auto at = isize(0);
    auto const size = text.size();
    auto const digits = [&]
    {
        auto const start = at;
        while (at < size && (is_digit(text[at]) || text[at] == '\''))
            ++at;
        return at > start;
    };

    if (at < size && (text[at] == '-' || text[at] == '+'))
        ++at;

    // NUM-10: `0x` and `0b` spell an integer like any other, held to the type asked of it (CHK-269)
    if (at + 1 < size && text[at] == '0' && (text[at + 1] == 'x' || text[at + 1] == 'b'))
    {
        auto const is_hex = text[at + 1] == 'x';
        at += 2;
        auto const start = at;
        while (at < size
               && (text[at] == '\'' || (text[at] >= '0' && text[at] <= '1')
                   || (is_hex
                       && (is_digit(text[at]) || (text[at] >= 'a' && text[at] <= 'f')
                           || (text[at] >= 'A' && text[at] <= 'F')))))
            ++at;
        return at > start && at == size ? number_class::plain_integer : number_class::other;
    }

    if (!digits())
        return number_class::other;
    if (at == size)
        return number_class::plain_integer;

    auto is_float = false;
    if (text[at] == '.')
    {
        is_float = true;
        ++at;
        digits();
    }
    if (at < size && text[at] == 'e')
    {
        is_float = true;
        ++at;
        if (at < size && (text[at] == '-' || text[at] == '+'))
            ++at;
        if (!digits())
            return number_class::other;
    }
    return is_float && at == size ? number_class::plain_float : number_class::other;
}
} // namespace

cc::optional<suffixed_number> impl::split_suffix(cc::string_view text)
{
    auto const is_digit = [](char c) { return c >= '0' && c <= '9'; };
    auto end = text.size();
    while (end > 0 && is_digit(text[end - 1]))
        --end;
    if (end < 2)
        return {};
    auto const letter = text[end - 1];
    if (letter != 'i' && letter != 'u' && letter != 'f')
        return {};
    auto const body = text.subview({.offset = 0, .size = end - 1});
    // NUM-16: `f` is a digit of a hexadecimal number
    auto const unsigned_body = body.starts_with('-') || body.starts_with('+') ? body.subview(1) : body;
    if (letter == 'f' && (unsigned_body.starts_with("0x") || unsigned_body.starts_with("0X")))
        return {};
    auto const kind = classify_unsuffixed(body);
    if (kind != number_class::plain_integer && kind != number_class::plain_float)
        return {};
    auto result = suffixed_number{.body = body, .letter = letter};
    if (end < text.size())
    {
        // a width this long names no type either way, and is kept from overflowing
        result.width = 0;
        for (auto i = end; i < text.size() && result.width < 100'000; ++i)
            result.width = result.width * 10 + (text[i] - '0');
    }
    return result;
}

number_class impl::classify_number(cc::string_view text)
{
    if (split_suffix(text).has_value())
        return number_class::suffixed;
    return classify_unsuffixed(text);
}

cc::optional<f64> impl::parse_plain_float(cc::string_view text)
{
    auto plain = cc::string();
    for (auto const c : text)
        if (c != '\'' && c != '+')
            plain += c;
    // an exponent's `+` went with the rest, and `1e+6` is `1e6`
    return cc::from_string<f64>(plain);
}

bool impl::is_hex_literal(cc::string_view text)
{
    return text.starts_with("0x");
}

namespace
{
/// The magnitude `text` spells behind its sign, which must be `limit` or below.
cc::optional<u64> parse_magnitude(cc::string_view text, u64 limit)
{
    auto at = isize(0);
    if (!text.empty() && (text[0] == '-' || text[0] == '+'))
        ++at;
    auto base = u64(10);
    if (at + 1 < text.size() && text[at] == '0' && (text[at + 1] == 'x' || text[at + 1] == 'b'))
    {
        base = text[at + 1] == 'x' ? 16 : 2;
        at += 2;
    }

    auto magnitude = u64(0);
    auto has_digit = false;
    for (; at < text.size(); ++at)
    {
        auto const c = text[at];
        if (c == '\'')
            continue;
        auto digit = u64(0);
        if (c >= '0' && c <= '9')
            digit = u64(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = u64(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = u64(c - 'A' + 10);
        else
            return cc::nullopt;
        if (digit >= base || magnitude > (limit - digit) / base)
            return cc::nullopt;
        magnitude = magnitude * base + digit;
        has_digit = true;
    }
    if (!has_digit)
        return cc::nullopt;
    return magnitude;
}
} // namespace

cc::optional<i64> impl::parse_literal_integer(cc::string_view text)
{
    auto const is_negative = !text.empty() && text[0] == '-';
    // The magnitude of the most negative i64 is one past the largest positive one.
    auto const magnitude = parse_magnitude(text, is_negative ? u64(1) << 63 : (u64(1) << 63) - 1);
    if (!magnitude.has_value())
        return cc::nullopt;
    return is_negative ? i64(0u - magnitude.value()) : i64(magnitude.value());
}

cc::optional<u64> impl::parse_literal_past_long(cc::string_view text)
{
    if (text.empty() || text[0] == '-')
        return cc::nullopt;
    auto const magnitude = parse_magnitude(text, ~u64(0));
    if (!magnitude.has_value() || magnitude.value() < u64(1) << 63)
        return cc::nullopt;
    return magnitude;
}

cc::optional<i32> impl::parse_plain_integer(cc::string_view text)
{
    auto const value = parse_literal_integer(text);
    if (!value.has_value() || value.value() < -2147483647 - 1 || value.value() > 2147483647)
        return cc::nullopt;
    return i32(value.value());
}

// ---- shared helpers -------------------------------------------------------------------------------------------------

source_span checker::span_of(i32 file, form_id form) const
{
    return sgl::is_valid(form) ? file_of(file).at(form).where : source_span{};
}

source_span checker::span_of(i32 file, ast::expr_id expr) const
{
    return ast::is_valid(expr) ? span_of(file, ast_of(file).at(expr).form) : source_span{};
}

source_span checker::span_of(i32 file, ast::decl_id decl) const
{
    return ast::is_valid(decl) ? span_of(file, ast_of(file).at(decl).form) : source_span{};
}

source_span checker::span_of(i32 file, ast::stmt_id stmt) const
{
    return ast::is_valid(stmt) ? span_of(file, ast_of(file).at(stmt).form) : source_span{};
}

located_diagnostic& checker::report(diagnostic_kind kind, i32 file, source_span where, cc::string detail)
{
    auto const is_error = default_severity_of(kind) != severity::warning;
    // CHK-338: a generic body with its type parameters open is judged for its names alone
    if (generic_passes > 0 && kind != diagnostic_kind::unknown_name && kind != diagnostic_kind::wrong_kind_of_name)
    {
        dropped = {};
        return dropped;
    }
    if (is_error)
        ++error_events;
    if (!instantiating.empty())
    {
        // the same error from another instantiation is reported once, under the instantiation that found it first
        for (auto& d : out.diagnostics)
            if (d.what.kind == kind && d.file == file && d.what.where == where && d.detail == detail)
                return d;
        auto& d = out.diagnostics.push_back({
            .what = {.kind = kind, .level = default_severity_of(kind), .where = where},
            .file = file,
            .detail = cc::move(detail),
        });
        for (auto i = instantiating.size(); i > 0; --i)
        {
            auto const& at = instantiating[i - 1];
            auto const& instance = out.function_instances[at.instance];
            auto const& info = out.functions[out.at(instance.function).info];
            auto arguments = cc::string();
            for (auto const parameter : out.at(info.type_parameters))
            {
                auto const pairs = out.at(instance.bindings);
                for (auto k = isize(0); k + 1 < pairs.size(); k += 2)
                    if (pairs[k] == parameter)
                        arguments.appendf("{}{}", arguments.empty() ? "" : ", ", out.name_of(pairs[k + 1]));
            }
            d.notes.push_back(
                {.file = at.file,
                 .where = at.where,
                 .message = cc::format("in {}[{}], instantiated here", out.at(instance.function).name, arguments)});
        }
        return d;
    }
    out.diagnostics.push_back({
        .what = {.kind = kind, .level = default_severity_of(kind), .where = where},
        .file = file,
        .detail = cc::move(detail),
    });
    return out.diagnostics.back();
}

void checker::tell_apart(located_diagnostic& d, type_id expected, type_id got)
{
    if (expected == got || out.name_of(expected) != out.name_of(got))
        return;
    d.detail += ", two different types of that name";
    auto const note_at = [&](type_id t, cc::string message)
    {
        auto const symbol = out.at(t).symbol;
        if (!is_valid(symbol) || index_of(symbol) >= out.symbols.size())
            return;
        auto const& s = out.at(symbol);
        auto const& node = ast_of(s.file).at(s.declaration).node;
        auto const* const structure = node.try_as<ast::struct_decl>();
        auto const* const enumeration = node.try_as<ast::enum_decl>();
        if (structure == nullptr && enumeration == nullptr)
            return;
        auto const where = structure != nullptr ? structure->name : enumeration->name;
        d.notes.push_back({.file = s.file, .where = where, .message = cc::move(message)});
    };
    note_at(expected, cc::format("the {} expected is declared here", out.name_of(expected)));
    note_at(got, cc::format("the {} given is declared here", out.name_of(got)));
}

void checker::unsupported(i32 file, source_span where, cc::string_view construct)
{
    report(diagnostic_kind::unsupported_yet, file, where, construct);
}

located_diagnostic& checker::report_once(diagnostic_kind kind, i32 file, source_span where, cc::string_view detail)
{
    for (auto& d : out.diagnostics)
        if (d.what.kind == kind && d.file == file && d.what.where == where)
            return d;
    return report(kind, file, where, cc::string(detail));
}

isize checker::error_count() const
{
    auto count = isize(0);
    for (auto const& d : out.diagnostics)
        if (d.what.level != severity::warning)
            ++count;
    return count;
}

ast::attribute const* checker::find_attribute(i32 file, ast::range_of<ast::attribute> range, cc::string_view name) const
{
    // most declarations carry none, and this is asked many names per declaration
    if (range.empty())
        return nullptr;
    for (auto const& a : ast_of(file).at(range))
        if (text_of(file, a.name) == name)
            return &a;
    return nullptr;
}

void checker::judge_attributes(i32 file,
                               ast::range_of<ast::attribute> range,
                               cc::span<cc::string_view const> known,
                               cc::string_view owner,
                               setting_scope scope)
{
    for (auto const& a : ast_of(file).at(range))
    {
        auto const name = text_of(file, a.name);
        auto is_known = false;
        for (auto const k : known)
            is_known = is_known || k == name;

        // A setting's value is judged by each pipeline that reads it.
        if (!is_known && scope != setting_scope::none && is_setting_attribute(name, scope == setting_scope::target))
            continue;
        if (!is_known && scope != setting_scope::none)
        {
            // An attribute has no path to disambiguate with, so the pipeline has to set the field itself.
            auto const paths = setting_attribute_paths(name, scope == setting_scope::target);
            if (paths.size() > 1)
            {
                report(diagnostic_kind::invalid_pipeline, file, a.name,
                       cc::format("@{} names both {} and {}: set it in the pipeline by its whole path", name, paths[0],
                                  paths[1]));
                continue;
            }
        }
        if (!is_known)
            unsupported(file, a.name, cc::format("the attribute @{} on {}", name, owner));
        // CHK-220: its one argument is `false` or `true`, whatever it stands on
        else if (name == "shadowable")
        {
            auto const arguments = ast_of(file).at(a.arguments);
            auto const text = arguments.size() == 1 && ast::is_valid(arguments[0].value) && arguments[0].name.empty()
                                ? text_of(file, span_of(file, arguments[0].value))
                                : cc::string_view();
            if (text != "false" && text != "true")
                report(diagnostic_kind::invalid_attribute_arguments, file, a.name,
                       "@shadowable takes `false` or `true`, as in @shadowable(false)");
        }
        // CHK-389: a count asks for partial unrolling
        // CHK-414: `false` asks for the loop to stay one
        else if (sgl::is_valid(a.list) && name == "unroll")
        {
            auto const arguments = ast_of(file).at(a.arguments);
            auto const is_false = arguments.size() == 1 && ast::is_valid(arguments[0].value) && arguments[0].name.empty()
                               && text_of(file, span_of(file, arguments[0].value)) == "false";
            if (!is_false)
                unsupported(file, span_of(file, a.list), "@unroll with arguments, which would unroll a loop in part");
        }
        else if (sgl::is_valid(a.list) && name != "operator" && name != "compute" && name != "preferred_subgroup_size"
                 && name != "float" && name != "stream" && name != "bits" && name != "stages" && name != "shadowable"
                 && name != "expect" && name != "interpolate" && name != "format" && name != "depth"
                 && name != "sampler" && name != "geometry" && name != "tessellation_control" && name != "mesh"
                 && name != "task" && name != "padding" && name != "swizzle" && name != "forward" && name != "backward")
            report(diagnostic_kind::invalid_attribute_arguments, file, span_of(file, a.list),
                   cc::format("@{} takes no arguments", name));
    }
}

void checker::set_type(i32 file, ast::expr_id expr, type_id type)
{
    if (!ast::is_valid(expr))
        return;
    if (current_instance >= 0 && file == out.at(out.function_instances[current_instance].function).file)
        out.function_instances.own(current_instance).tables.type_of[own_slot(expr)] = type;
    else
        out.files.own(file).type_of[ast::index_of(expr)] = type;
}

void checker::set_target(i32 file, ast::expr_id expr, target where)
{
    if (!ast::is_valid(expr))
        return;
    if (current_instance >= 0 && file == out.at(out.function_instances[current_instance].function).file)
        out.function_instances.own(current_instance).tables.target_of[own_slot(expr)] = where;
    else
        out.files.own(file).target_of[ast::index_of(expr)] = where;
}

table_view checker::tables_of(i32 file) const
{
    auto const is_own = current_instance >= 0 && file == out.at(out.function_instances[current_instance].function).file;
    return {.module = &out, .file = file, .instance = is_own ? current_instance : -1};
}

isize checker::own_slot(ast::expr_id expr)
{
    auto& instance = out.function_instances.own(current_instance);
    auto& t = instance.tables;
    auto const index = isize(ast::index_of(expr));
    if (t.type_of.empty())
        instance.first = i32(index);
    // grows to the front as well, since a body need not write its first expression first
    if (index < instance.first)
    {
        auto const grow = instance.first - index;
        auto grown = file_tables{
            .type_of = cc::vector<type_id>::create_filled(grow, type_id::none),
            .target_of = cc::vector<target>::create_filled(grow, target{}),
            .call_of = cc::vector<i32>::create_filled(grow, -2),
        };
        grown.type_of.push_back_range(t.type_of);
        grown.target_of.push_back_range(t.target_of);
        grown.call_of.push_back_range(t.call_of);
        t = cc::move(grown);
        instance.first = i32(index);
    }
    auto const slot = index - instance.first;
    while (t.type_of.size() <= slot)
    {
        t.type_of.push_back(type_id::none);
        t.target_of.push_back(target{});
        t.call_of.push_back(-2);
    }
    return slot;
}
