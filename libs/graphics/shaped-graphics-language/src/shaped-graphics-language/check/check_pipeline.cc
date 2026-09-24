#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
constexpr cc::string_view k_description = "raster_pipeline_description";
constexpr cc::string_view k_color_targets = "color_targets";
/// Stands in a resolved path where `color_targets` fans out to every target.
constexpr cc::string_view k_every_target = "*";

/// One name of a setting's path, and where it was written.
struct path_name
{
    cc::string_view name;
    source_span where;
};

/// A path down the description, from its root to the field it names.
struct resolved_path
{
    /// With a target's member name, or `k_every_target`, after `color_targets`.
    cc::vector<cc::string> names;
    /// The type of the field it ends at.
    type_id leaf = type_id::none;
};

/// Everything written from one place: one edge struct, one stage, or the declaration.
struct settings_source
{
    setting_source kind = setting_source::declaration;
    cc::vector<pipeline_setting> settings;
};

[[nodiscard]] cc::string joined(cc::span<cc::string const> names)
{
    auto result = cc::string();
    for (auto const& n : names)
    {
        if (!result.empty())
            result += ".";
        result += n;
    }
    return result;
}

[[nodiscard]] bool same_value(pipeline_setting const& a, pipeline_setting const& b)
{
    return a.kind == b.kind && a.integer == b.integer && a.real == b.real && a.enum_case == b.enum_case;
}

/// Where `.host` may stand: a target's format, the depth-stencil format, the sample count.
[[nodiscard]] bool is_open_field(cc::span<cc::string const> names)
{
    if (names.empty())
        return false;
    auto const& last = names.back();
    if (last == "depth_stencil_format" || last == "sample_count")
        return names.size() == 1;
    return last == "format" && names.size() == 3 && names[0] == k_color_targets;
}

[[nodiscard]] bool is_optional_field(cc::span<cc::string const> names)
{
    return names.size() == 3 && names[0] == k_color_targets && names[2] == "blend";
}

/// Why `value` is out of range for the int field at `path`, or empty where it is in range.
/// sg holds these narrower than SGL's `int`, so a value outside is truncated or refused past this point.
[[nodiscard]] cc::string range_error(cc::string_view path, i64 value)
{
    if (path == "depth_stencil.stencil_read_mask" || path == "depth_stencil.stencil_write_mask")
        return value >= 0 && value <= 255 ? cc::string() : cc::format("{} is a byte: 0 to 255", path);
    if (path == "sample_count")
        return value >= 1 && value <= 64 && (value & (value - 1)) == 0
                 ? cc::string()
                 : cc::format("{} is a power of two from 1 to 64", path);
    if (path == "patch_control_points")
        return value >= 0 && value <= 32 ? cc::string() : cc::format("{} is 0 to 32", path);
    return {};
}
} // namespace

// ---- the description ------------------------------------------------------------------------------------------------

type_id checker::pipeline_description_type()
{
    auto const* const found = names.get_ptr(k_description);
    if (found == nullptr || found->empty() || out.at(found->front()).kind != symbol_kind::structure)
        return checked_module::error_type;
    auto const id = found->front();
    if (demand(id, out.at(id).file, {}) != symbol_state::checked)
        return checked_module::error_type;
    return out.at(id).type;
}

namespace
{
/// Every path below `type` that ends at a field named `name`, into `found`.
/// A path through `color_targets` goes through every target at once.
void find_fields(checked_module const& out,
                 type_id type,
                 cc::string_view name,
                 cc::vector<cc::string>& prefix,
                 cc::vector<resolved_path>& found)
{
    auto const& info = out.at(type);
    if (info.kind != type_kind::structure || sgl::is_valid(out.at(info.symbol).intrinsic_type))
        return;
    for (auto const& m : out.at(info.members))
    {
        prefix.push_back(m.name);
        if (m.name == k_color_targets)
            prefix.push_back(cc::string(k_every_target));
        if (m.name == name)
            found.push_back({.names = prefix, .leaf = m.type});
        else
            find_fields(out, m.type, name, prefix, found);
        if (m.name == k_color_targets)
            prefix.remove_back();
        prefix.remove_back();
    }
}

[[nodiscard]] member_info const* field_named(checked_module const& out, type_id type, cc::string_view name)
{
    auto const& info = out.at(type);
    if (info.kind != type_kind::structure)
        return nullptr;
    for (auto const& m : out.at(info.members))
        if (m.name == name)
            return &m;
    return nullptr;
}
} // namespace

bool checker::is_setting_attribute(cc::string_view name, bool is_on_target)
{
    // A stage marks an entry point or an edge struct, and fills a stage slot; it is never a field.
    if (name == "vertex" || name == "pixel" || name == "compute")
        return false;
    auto const description = pipeline_description_type();
    if (description == checked_module::error_type)
        return false;
    auto const* const targets = field_named(out, description, k_color_targets);
    auto const root = is_on_target && targets != nullptr ? targets->type : description;
    auto prefix = cc::vector<cc::string>();
    auto found = cc::vector<resolved_path>();
    find_fields(out, root, name, prefix, found);
    return found.size() == 1;
}

// ---- a pipeline -----------------------------------------------------------------------------------------------------

namespace
{
/// What one `compile_pipeline` works on; the settings it collects stay local until the pipeline checked.
struct pipeline_compiler
{
    checker& c;
    /// The declaration's file; attribute defaults carry their own.
    i32 file = 0;
    type_id description = type_id::none;
    /// The `@pixel struct` members, in location order.
    cc::vector<cc::string> targets;
    bool is_failed = false;

    void fail(i32 in_file, source_span where, cc::string detail)
    {
        c.report(diagnostic_kind::invalid_pipeline, in_file, where, cc::move(detail));
        is_failed = true;
    }

    /// A name or a `member` chain, as its names from the left.
    [[nodiscard]] bool names_of(i32 in_file, ast::expr_id expr, cc::vector<path_name>& names)
    {
        if (!ast::is_valid(expr))
            return false;
        auto const& e = c.ast_of(in_file).at(expr);
        if (auto const* const n = e.node.try_as<ast::name>())
        {
            names.push_back({.name = c.text_of(in_file, n->where), .where = n->where});
            return true;
        }
        if (auto const* const m = e.node.try_as<ast::member>())
        {
            if (!names_of(in_file, m->object, names))
                return false;
            names.push_back({.name = c.text_of(in_file, m->name), .where = m->name});
            return true;
        }
        return false;
    }

    /// The field a setting's path names, from `root`; nullopt after a report.
    /// `prefix` is where `root` stands in the description: empty for the description, a target's path for one target.
    [[nodiscard]] cc::optional<resolved_path> resolve(i32 in_file,
                                                      cc::span<path_name const> names,
                                                      type_id root,
                                                      cc::vector<cc::string> prefix)
    {
        auto result = resolved_path{.names = cc::move(prefix), .leaf = root};
        for (auto i = isize(0); i < names.size(); ++i)
        {
            auto const& n = names[i];
            auto const* const direct = field_named(c.out, result.leaf, n.name);

            // The first name alone may stand for a field below the one it starts from, while that field is unique.
            if (direct == nullptr && i == 0)
            {
                auto found = cc::vector<resolved_path>();
                auto lifted_prefix = cc::vector<cc::string>();
                find_fields(c.out, result.leaf, n.name, lifted_prefix, found);
                if (found.empty())
                {
                    fail(in_file, n.where, cc::format("no setting is named {}", n.name));
                    return cc::nullopt;
                }
                if (found.size() > 1)
                {
                    fail(in_file, n.where,
                         cc::format("{} names both {} and {}: write the whole path", n.name, joined(found[0].names),
                                    joined(found[1].names)));
                    return cc::nullopt;
                }
                result.names.push_back_range(found[0].names);
                result.leaf = found[0].leaf;
                continue;
            }
            if (direct == nullptr)
            {
                fail(in_file, n.where, cc::format("{} has no field {}", c.out.name_of(result.leaf), n.name));
                return cc::nullopt;
            }

            result.names.push_back(direct->name);
            result.leaf = direct->type;

            // `color_targets` is one entry per target, so the name after it is a target's.
            if (direct->name == k_color_targets)
            {
                if (i + 1 >= names.size())
                {
                    fail(in_file, n.where, "a target is set by its member name: `color_targets.<target>.format`");
                    return cc::nullopt;
                }
                auto const& target = names[i + 1];
                auto is_target = false;
                for (auto const& t : targets)
                    is_target = is_target || t == target.name;
                if (!is_target)
                {
                    fail(in_file, target.where,
                         targets.empty() ? cc::format("this pipeline has no targets, so none is named {}", target.name)
                                         : cc::format("the pixel stage has no target {}", target.name));
                    return cc::nullopt;
                }
                result.names.push_back(cc::string(target.name));
                ++i;
            }
        }
        return result;
    }

    /// Appends the settings `value` writes to the field at `path`, one per leaf; false after a report.
    [[nodiscard]] bool evaluate(i32 in_file,
                                ast::expr_id value,
                                resolved_path const& path,
                                setting_source source,
                                source_span where,
                                cc::vector<pipeline_setting>& into)
    {
        auto const& ast = c.ast_of(in_file);
        if (!ast::is_valid(value) || ast.at(value).node.is<ast::invalid_expr>())
        {
            is_failed = true;
            return false;
        }
        auto const& e = ast.at(value).node;
        auto const at = c.span_of(in_file, value);
        auto const leaf = path.leaf;
        auto const& type = c.out.at(leaf);
        auto const* const builtin = c.out.builtin_type_of(leaf);
        auto const field = joined(path.names);
        auto setting = pipeline_setting{.path = field, .source = source, .file = in_file, .where = where};

        auto const* const dot = e.try_as<ast::leading_dot>();
        if (dot != nullptr && c.text_of(in_file, dot->name) == "host")
        {
            if (!is_open_field(path.names))
            {
                fail(in_file, at, cc::format("{} is no format and no sample count, so the host cannot state it", field));
                return false;
            }
            setting.kind = setting_kind::host;
            into.push_back(cc::move(setting));
            return true;
        }
        if (dot != nullptr && c.text_of(in_file, dot->name) == "none" && is_optional_field(path.names))
        {
            setting.kind = setting_kind::none;
            into.push_back(cc::move(setting));
            return true;
        }

        if (type.kind == type_kind::enumeration)
        {
            if (dot == nullptr)
            {
                fail(in_file, at,
                     cc::format("{} is a {}: name a case, as in `.{}`", field, c.out.name_of(leaf),
                                c.out.at(type.cases).front().name));
                return false;
            }
            auto const name = c.text_of(in_file, dot->name);
            for (auto const& k : c.out.at(type.cases))
                if (k.name == name)
                {
                    setting.kind = setting_kind::enum_case;
                    setting.enum_case = k.name;
                    into.push_back(cc::move(setting));
                    return true;
                }
            c.report(diagnostic_kind::unknown_member, in_file, at,
                     cc::format("the enum {} has no case {}", c.out.name_of(leaf), name));
            is_failed = true;
            return false;
        }

        if (builtin != nullptr && builtin->name == builtins::k_bool)
        {
            auto const* const n = e.try_as<ast::name>();
            auto const text = n != nullptr ? c.text_of(in_file, n->where) : cc::string_view();
            if (text != "true" && text != "false")
            {
                fail(in_file, at, cc::format("{} is a bool: `true` or `false`", field));
                return false;
            }
            setting.kind = setting_kind::boolean;
            setting.integer = text == "true" ? 1 : 0;
            into.push_back(cc::move(setting));
            return true;
        }

        if (builtin != nullptr && (builtin->name == "int" || builtin->name == "float"))
        {
            // A negative number is a prefix call of `-` on a literal.
            auto number = value;
            auto is_negative = false;
            if (auto const* const call = e.try_as<ast::call>();
                call != nullptr && call->spelling == ast::call_spelling::prefix && sgl::is_valid(call->op)
                && ast.at(call->arguments).size() == 1)
            {
                is_negative = c.text_of(in_file, c.file_of(in_file).at(call->op).where) == "-";
                number = ast.at(call->arguments)[0].value;
            }
            auto const is_literal = ast::is_valid(number) && ast.at(number).node.is<ast::literal>()
                                 && (!e.is<ast::call>() || is_negative);
            auto const text = is_literal ? c.text_of(in_file, c.span_of(in_file, number)) : cc::string_view();
            auto const kind = is_literal ? classify_number(text) : number_class::other;
            if (builtin->name == "int" && kind == number_class::plain_integer)
            {
                auto const parsed = parse_plain_integer(text);
                if (parsed.has_value())
                {
                    setting.kind = setting_kind::integer;
                    setting.integer = is_negative ? -i64(parsed.value()) : i64(parsed.value());
                    if (auto const range = range_error(field, setting.integer); !range.empty())
                    {
                        fail(in_file, at, range);
                        return false;
                    }
                    into.push_back(cc::move(setting));
                    return true;
                }
            }
            if (builtin->name == "float" && (kind == number_class::plain_integer || kind == number_class::plain_float))
            {
                auto const parsed = parse_plain_float(text);
                if (parsed.has_value())
                {
                    setting.kind = setting_kind::real;
                    setting.real = is_negative ? -parsed.value() : parsed.value();
                    into.push_back(cc::move(setting));
                    return true;
                }
            }
            fail(in_file, at,
                 cc::format("{} is {} {}: write a number", field, builtin->name == "int" ? "an" : "a", builtin->name));
            return false;
        }

        if (type.kind == type_kind::structure && builtin == nullptr)
        {
            // A paren literal replaces the whole part, so it names every field of it.
            auto elements = ast::range_of<ast::argument>();
            if (auto const* const t = e.try_as<ast::tuple>())
                elements = t->elements;
            else if (auto const* const o = e.try_as<ast::object>())
                elements = o->elements;
            else
            {
                fail(in_file, at,
                     cc::format("{} is a {}: write every field, as in `({} = ...)`", field, c.out.name_of(leaf),
                                c.out.at(type.members).front().name));
                return false;
            }

            auto const members = c.out.at(type.members);
            auto seen = cc::vector<bool>::create_filled(members.size(), false);
            auto is_ok = true;
            for (auto const& element : ast.at(elements))
            {
                auto const element_where = c.span_of(in_file, element.form);
                if (element.name.empty() || element.is_splat)
                {
                    fail(in_file, element_where, "every field of a part is named: `(field = value, ...)`");
                    is_ok = false;
                    continue;
                }
                auto const name = c.text_of(in_file, element.name);
                auto index = isize(-1);
                for (auto i = isize(0); i < members.size(); ++i)
                    if (members[i].name == name)
                        index = i;
                if (index < 0)
                {
                    c.report(diagnostic_kind::unknown_field, in_file, element.name,
                             cc::format("{} has no field {}", c.out.name_of(leaf), name));
                    is_failed = true;
                    is_ok = false;
                    continue;
                }
                if (seen[index])
                {
                    c.report(diagnostic_kind::duplicate_field, in_file, element.name, name);
                    is_failed = true;
                    is_ok = false;
                    continue;
                }
                seen[index] = true;
                auto inner = path;
                inner.names.push_back(members[index].name);
                inner.leaf = members[index].type;
                is_ok = evaluate(in_file, element.value, inner, source, where, into) && is_ok;
            }
            for (auto i = isize(0); i < members.size(); ++i)
                if (!seen[i])
                {
                    c.report(
                        diagnostic_kind::missing_field, in_file, at,
                        cc::format("{}: a part is written whole, or one field at a time by its path", members[i].name));
                    is_failed = true;
                    is_ok = false;
                }
            return is_ok;
        }

        c.unsupported(in_file, at, "this value in a pipeline setting");
        is_failed = true;
        return false;
    }

    /// `path = value` from the description's root, or from one target's; appends one setting per leaf and target.
    void assign(i32 in_file,
                cc::span<path_name const> names,
                type_id root,
                cc::vector<cc::string> prefix,
                ast::expr_id value,
                setting_source source,
                source_span where,
                cc::vector<pipeline_setting>& into)
    {
        auto const path = resolve(in_file, names, root, cc::move(prefix));
        if (!path.has_value())
            return;

        // A field under `color_targets` without a target's name is every target's.
        auto is_every_target = false;
        for (auto const& n : path.value().names)
            is_every_target = is_every_target || n == k_every_target;
        if (!is_every_target)
        {
            (void)evaluate(in_file, value, path.value(), source, where, into);
            return;
        }
        if (targets.empty())
        {
            fail(in_file, where, "this pipeline has no targets, and the setting is one per target");
            return;
        }
        for (auto const& target : targets)
        {
            auto one = path.value();
            for (auto& n : one.names)
                if (n == k_every_target)
                    n = target;
            if (!evaluate(in_file, value, one, source, where, into))
                return;
        }
    }

    /// The attributes of a stage or an edge struct that are settings, as that source's settings.
    void attribute_settings(i32 in_file,
                            ast::range_of<ast::attribute> attributes,
                            type_id root,
                            cc::vector<cc::string> const& prefix,
                            bool is_on_target,
                            setting_source source,
                            cc::vector<pipeline_setting>& into)
    {
        for (auto const& a : c.ast_of(in_file).at(attributes))
        {
            auto const name = c.text_of(in_file, a.name);
            if (!c.is_setting_attribute(name, is_on_target))
                continue;
            auto const arguments = c.ast_of(in_file).at(a.arguments);
            if (arguments.size() != 1 || !arguments[0].name.empty() || arguments[0].is_splat)
            {
                fail(in_file, a.name, cc::format("a setting as an attribute takes one value: `@{}(value)`", name));
                continue;
            }
            path_name const names[] = {{.name = name, .where = a.name}};
            assign(in_file, names, root, prefix, arguments[0].value, source, a.name, into);
        }
    }
};
} // namespace

void checker::compile_pipeline(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const decl = out.at(id).declaration;
    auto const& ast = ast_of(file);
    auto const& d = ast.at(decl);
    auto const& p = d.node.as<ast::pipeline_decl>();
    auto const where = span_of(file, decl);
    auto const fail_symbol = [&] { out.symbols[index_of(id)].state = symbol_state::failed; };

    cc::string_view const known[] = {"raster", "compute", "raytracing"};
    judge_attributes(file, d.attributes, known, "a pipeline");
    if (auto const* const a = find_attribute(file, d.attributes, "compute"))
    {
        unsupported(file, a->name, "a @compute pipeline; every compute entry point is its own");
        return fail_symbol();
    }
    if (auto const* const a = find_attribute(file, d.attributes, "raytracing"))
    {
        unsupported(file, a->name, "a @raytracing pipeline");
        return fail_symbol();
    }

    auto pc = pipeline_compiler{.c = *this, .file = file, .description = pipeline_description_type()};
    if (pc.description == checked_module::error_type)
    {
        report(diagnostic_kind::unknown_name, file, where,
               cc::format("{}, which the prelude must declare", k_description));
        return fail_symbol();
    }

    // ---- the stages
    auto vertex = symbol_id::none;
    auto pixel = symbol_id::none;
    auto const place_stage = [&](ast::expr_id value, stage slot, source_span at) -> bool
    {
        auto const* const n = ast::is_valid(value) ? ast.at(value).node.try_as<ast::name>() : nullptr;
        if (n == nullptr)
        {
            pc.fail(file, at, "a stage is filled by the name of an entry point");
            return false;
        }
        auto const text = text_of(file, n->where);
        auto const* const found = names.get_ptr(text);
        if (found == nullptr || found->empty())
        {
            report(diagnostic_kind::unknown_name, file, n->where, text);
            pc.is_failed = true;
            return false;
        }
        auto entry = symbol_id::none;
        auto entries = 0;
        auto is_silent = false;
        for (auto const candidate : *found)
        {
            if (out.at(candidate).kind != symbol_kind::function)
                continue;
            if (demand(candidate, file, n->where) != symbol_state::checked)
            {
                is_silent = true;
                continue;
            }
            if (out.functions[out.at(candidate).info].entry_stage != stage::none)
            {
                entry = candidate;
                ++entries;
            }
        }
        if (entries != 1)
        {
            // an entry point that failed was reported where it is declared
            if (!is_silent)
                pc.fail(file, n->where,
                        entries == 0 ? cc::format("{} is no entry point", text)
                                     : cc::format("{} names {} entry points", text, entries));
            pc.is_failed = true;
            return false;
        }
        set_target(file, value, {.kind = target_kind::overload, .symbol = entry});
        auto const& info = out.functions[out.at(entry).info];
        if (!notes[out.at(entry).info].is_valid_entry)
        {
            pc.is_failed = true;
            return false;
        }
        auto const actual = slot == stage::none ? info.entry_stage : slot;
        if (info.entry_stage != actual)
        {
            pc.fail(file, n->where,
                    cc::format("{} is no {} entry point", text, actual == stage::vertex ? "@vertex" : "@pixel"));
            return false;
        }
        if (actual == stage::compute)
        {
            pc.fail(file, n->where, cc::format("{} is a compute entry point, which is a pipeline of its own", text));
            return false;
        }
        auto& into = actual == stage::vertex ? vertex : pixel;
        if (slot == stage::none && is_valid(into))
        {
            pc.fail(file, n->where,
                    cc::format("a pipeline has one {} stage", actual == stage::vertex ? "vertex" : "pixel"));
            return false;
        }
        into = entry;
        return true;
    };

    if (p.is_short_form)
    {
        for (auto const& element : ast.at(p.stages))
        {
            if (!element.name.empty() || element.is_splat)
            {
                pc.fail(file, span_of(file, element.form), "the short form lists entry points by name");
                continue;
            }
            (void)place_stage(element.value, stage::none, span_of(file, element.form));
        }
    }
    else
    {
        for (auto const& s : ast.at(p.settings))
        {
            auto path = cc::vector<path_name>();
            if (!pc.names_of(file, s.path, path) || path.size() != 1)
                continue;
            if (path[0].name == "vertex")
                (void)place_stage(s.value, stage::vertex, span_of(file, s.form));
            else if (path[0].name == "pixel")
                (void)place_stage(s.value, stage::pixel, span_of(file, s.form));
        }
    }
    if (!is_valid(vertex))
    {
        if (!pc.is_failed)
            pc.fail(file, where, "a pipeline has a vertex stage: `vertex = <entry point>`");
        return fail_symbol();
    }
    if (pc.is_failed)
        return fail_symbol();

    auto const& vertex_info = out.functions[out.at(vertex).info];
    auto const vertex_input = out.at(vertex_info.parameters)[0].type;
    auto target_set = type_id::none;

    // ---- the interface between the stages, and the targets
    if (is_valid(pixel))
    {
        auto const& pixel_info = out.functions[out.at(pixel).info];
        auto const passed = vertex_info.result;
        auto const taken = out.at(pixel_info.parameters)[0].type;
        auto const returned = out.at(out.at(passed).members);
        auto const expected = out.at(out.at(taken).members);
        auto mismatch = cc::string();
        if (returned.size() != expected.size())
            mismatch = cc::format("{} passes {} members and {} takes {}", out.at(vertex).name, returned.size(),
                                  out.at(pixel).name, expected.size());
        else
            for (auto i = isize(0); i < returned.size() && mismatch.empty(); ++i)
            {
                auto const& a = returned[i];
                auto const& b = expected[i];
                if (a.name != b.name || a.type != b.type || a.is_position != b.is_position)
                    mismatch = cc::format("member {} is `{}: {}` where {} returns it and `{}: {}` where {} takes it", i,
                                          a.name, out.name_of(a.type), out.at(vertex).name, b.name, out.name_of(b.type),
                                          out.at(pixel).name);
            }
        if (!mismatch.empty())
            pc.fail(file, where, cc::format("the stages pass one interface, member for member: {}", mismatch));

        target_set = pixel_info.result;
        for (auto const& m : out.at(out.at(target_set).members))
        {
            // The host states an open target's format by the target's name, beside these two.
            if (m.name == "sample_count" || m.name == "depth_stencil_format")
                pc.fail(
                    file, where,
                    cc::format("a target of a pipeline is not named {}, which the pipeline's own setting is", m.name));
            pc.targets.push_back(m.name);
        }
    }

    // ---- the binding lists
    auto layout = cc::vector<symbol_id>();
    auto inline_constants = symbol_id::none;
    {
        auto lists = cc::vector<cc::vector<symbol_id>>();
        for (auto const entry : {vertex, pixel})
        {
            if (!is_valid(entry))
                continue;
            auto list = cc::vector<symbol_id>();
            for (auto const b : out.at(out.functions[out.at(entry).info].bindings))
            {
                if (out.bindings[out.at(b).info].is_inline)
                {
                    if (is_valid(inline_constants) && inline_constants != b)
                        pc.fail(file, where,
                                cc::format("the stages list two @inline bindings, {} and {}, and a pipeline has one",
                                           out.at(inline_constants).name, out.at(b).name));
                    inline_constants = b;
                    continue;
                }
                list.push_back(b);
            }
            lists.push_back(cc::move(list));
        }
        for (auto const& list : lists)
            if (list.size() > layout.size())
                layout = list;
        for (auto const& list : lists)
            for (auto i = isize(0); i < list.size(); ++i)
                if (list[i] != layout[i])
                {
                    pc.fail(file, where,
                            cc::format("group {} is {} to one stage and {} to another: the binding lists agree by "
                                       "position",
                                       i, out.at(list[i]).name, out.at(layout[i]).name));
                    break;
                }
    }

    // ---- the settings, in the order they apply
    auto const* const targets_field = field_named(out, pc.description, k_color_targets);
    auto const target_state = targets_field != nullptr ? targets_field->type : checked_module::error_type;
    auto sources = cc::vector<settings_source>();

    auto const edge_source = [&](type_id edge, bool has_targets)
    {
        auto s = settings_source{.kind = setting_source::edge_struct};
        auto const& info = out.at(edge);
        auto const edge_file = out.at(info.symbol).file;
        auto const& edge_decl = ast_of(edge_file).at(out.at(info.symbol).declaration);
        pc.attribute_settings(edge_file, edge_decl.attributes, pc.description, {}, false, s.kind, s.settings);
        if (has_targets)
            for (auto const& m : out.at(info.members))
            {
                auto const prefix = cc::vector<cc::string>{cc::string(k_color_targets), m.name};
                pc.attribute_settings(edge_file, ast_of(edge_file).at(m.field).attributes, target_state, prefix, true,
                                      s.kind, s.settings);
            }
        sources.push_back(cc::move(s));
    };
    edge_source(vertex_input, false);
    if (is_valid(target_set))
        edge_source(target_set, true);

    for (auto const entry : {vertex, pixel})
    {
        if (!is_valid(entry))
            continue;
        auto s = settings_source{.kind = setting_source::stage};
        auto const entry_file = out.at(entry).file;
        auto const& entry_decl = ast_of(entry_file).at(out.at(entry).declaration);
        pc.attribute_settings(entry_file, entry_decl.attributes, pc.description, {}, false, s.kind, s.settings);
        sources.push_back(cc::move(s));
    }

    auto declared = settings_source{.kind = setting_source::declaration};
    for (auto const& s : ast.at(p.settings))
    {
        auto path = cc::vector<path_name>();
        if (!pc.names_of(file, s.path, path))
        {
            // a line that is no setting, and a left side that is no path, were reported by the AST pass
            pc.is_failed = true;
            continue;
        }
        if (path.size() == 1 && (path[0].name == "vertex" || path[0].name == "pixel"))
            continue;
        pc.assign(file, path, pc.description, {}, s.value, declared.kind, span_of(file, s.form), declared.settings);
    }

    // Two sources of one step disagree only where the declaration does not decide it.
    for (auto a = isize(0); a < sources.size(); ++a)
        for (auto b = a + 1; b < sources.size(); ++b)
        {
            if (sources[a].kind != sources[b].kind)
                continue;
            for (auto const& x : sources[a].settings)
                for (auto const& y : sources[b].settings)
                {
                    if (x.path != y.path || same_value(x, y))
                        continue;
                    auto is_decided = false;
                    for (auto const& z : declared.settings)
                        is_decided = is_decided || z.path == x.path;
                    if (!is_decided)
                        pc.fail(y.file, y.where,
                                cc::format("{} is set differently by two {}; the pipeline sets it to decide", x.path,
                                           x.source == setting_source::stage ? "stages" : "edge structs"));
                }
        }

    auto settings = cc::vector<pipeline_setting>();
    for (auto& s : sources)
        settings.push_back_range(s.settings);
    settings.push_back_range(declared.settings);

    // ---- every target has a format; after another error, a missing one is likely what that line meant to set
    for (auto const& t : pc.is_failed ? cc::span<cc::string const>() : cc::span<cc::string const>(pc.targets))
    {
        auto const path = cc::format("{}.{}.format", k_color_targets, t);
        auto const* last = static_cast<pipeline_setting const*>(nullptr);
        for (auto const& s : settings)
            if (s.path == path)
                last = &s;
        if (last == nullptr || (last->kind == setting_kind::enum_case && last->enum_case == "undefined"))
            pc.fail(file, where,
                    cc::format("the target {} has no format: set `color_targets.{}.format`, or leave it to the host "
                               "with "
                               "`.host`",
                               t, t));
    }

    if (pc.is_failed)
        return fail_symbol();

    out.symbols[index_of(id)].info = i32(out.pipelines.size());
    out.pipelines.push_back({
        .symbol = id,
        .vertex = vertex,
        .pixel = pixel,
        .layout = {.first = u32(out.binding_lists.size()), .count = u32(layout.size())},
        .inline_constants = inline_constants,
        .vertex_input = vertex_input,
        .target_set = target_set,
        .settings = {.first = u32(out.pipeline_settings.size()), .count = u32(settings.size())},
    });
    out.binding_lists.push_back_range(layout);
    out.pipeline_settings.push_back_range(settings);
}
