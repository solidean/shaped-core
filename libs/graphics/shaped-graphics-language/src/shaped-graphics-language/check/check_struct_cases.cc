#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Structs with cases (CHK-540 to CHK-551): a struct whose block declares cases, each a struct of its own that shares
// every field above it.
// A value is one carrier struct whatever its case, and the rest of the pipeline sees that carrier alone: the common
// fields, a tag where the compiler places one, and the variant region as `uint` words.

namespace
{
constexpr auto error_type = checked_module::error_type;

[[nodiscard]] u32 leaf_bit(i32 leaf)
{
    return u32(1) << leaf;
}

[[nodiscard]] bool holds_cases(ast::file_ast const& ast, ast::range_of<ast::decl_id> members)
{
    for (auto const member : ast.at(members))
        if (ast.at(member).node.is<ast::struct_case_decl>())
            return true;
    return false;
}

[[nodiscard]] i32 round_up(i32 n, i32 to)
{
    return to <= 0 ? n : (n + to - 1) / to * to;
}

/// A field a `@when` may read: one the struct's root declares, or one a case above it does.
struct readable_field
{
    cc::string_view name;
    type_id type = type_id::none;
    /// The member it is: of `members` for a root field, of `variant_fields` for a case's.
    i32 index = -1;
    bool is_variant = false;
    bool has_range = false;
    i64 range_first = 0;
    i64 range_last = 0;
};

} // namespace

bool checker::declares_cases(ast::file_ast const& ast, ast::struct_decl const& s)
{
    return holds_cases(ast, s.members);
}

void checker::declare_case_constructors(symbol_id owner, i32 file, ast::range_of<ast::decl_id> members)
{
    auto const& ast = ast_of(file);
    auto const walk = [&](auto const& self, ast::range_of<ast::decl_id> range) -> void
    {
        for (auto const member : ast.at(range))
        {
            auto const* const c = ast.at(member).node.try_as<ast::struct_case_decl>();
            if (c == nullptr || c->name.empty())
                continue;
            if (holds_cases(ast, c->members))
            {
                self(self, c->members);
                continue;
            }
            // CHK-545: a leaf is built by a function of the type scope of its name, which no other lookup finds
            auto const id = symbol_id(out.symbols.size());
            out.symbols.push_back({.file = file,
                                   .declaration = member,
                                   .kind = symbol_kind::function,
                                   .name = cc::string(text_of(file, c->name)),
                                   .role = function_role::case_constructor,
                                   .owner = owner});
            type_scopes[i32(index_of(owner))][out.at(id).name].push_back(id);
        }
    };
    walk(walk, members);
}

cc::string_view checker::case_member_kind(i32 file, ast::range_of<ast::decl_id> members, cc::string_view name) const
{
    auto const& ast = ast_of(file);
    for (auto const member : ast.at(members))
    {
        auto const* const c = ast.at(member).node.try_as<ast::struct_case_decl>();
        if (c == nullptr)
            continue;
        if (text_of(file, c->name) == name)
            return "case";
        for (auto const& f : ast.at(c->payload))
            if (!f.name.empty() && text_of(file, f.name) == name)
                return "field";
        for (auto const inner : ast.at(c->members))
            if (auto const* const fd = ast.at(inner).node.try_as<ast::field_decl>();
                fd != nullptr && ast::is_valid(fd->field) && text_of(file, ast.at(fd->field).name) == name)
                return "field";
        if (auto const found = case_member_kind(file, c->members, name); !found.empty())
            return found;
    }
    return {};
}

// ---- compiling the struct -------------------------------------------------------------------------------------------

void checker::compile_struct_cases(symbol_id id, type_id type, i32 file, ast::struct_decl const& s)
{
    auto const& ast = ast_of(file);
    auto const& d = ast.at(out.at(id).declaration);
    auto const uint_type = prelude_type(builtins::k_uint);
    auto const bool_type = type_of_builtin(builtins::k_bool, file, {});

    // CHK-540: what a struct with cases is not as well, each of which would need a layout of its own
    if (!s.branches.empty() || out.at(type).is_template || out.at(type).has_swizzles || out.at(type).edge != stage::none
        || s.is_ray_set)
    {
        unsupported(file, s.name,
                    "a struct with cases that is also a text type, a generic, a swizzled or an edge struct");
        return;
    }

    // ---- the tree of cases, each before the cases it holds -----------------------------------------------------------
    struct case_node
    {
        member_info info;
        ast::struct_case_decl const* decl = nullptr;
        ast::expr_id when = ast::expr_id::none;
    };
    auto cases = cc::vector<case_node>();
    auto leaf_count = 0;
    auto const walk = [&](auto const& self, ast::range_of<ast::decl_id> range, i32 parent) -> void
    {
        for (auto const member : ast.at(range))
        {
            auto const* const c = ast.at(member).node.try_as<ast::struct_case_decl>();
            if (c == nullptr || c->name.empty())
                continue;
            auto const name = text_of(file, c->name);
            auto is_duplicate = false;
            for (auto const& other : cases)
                is_duplicate = is_duplicate || other.info.name == name;
            if (is_duplicate)
            {
                report(diagnostic_kind::duplicate_declaration, file, c->name, cc::string(name));
                continue;
            }
            auto const& attributes = ast.at(member).attributes;
            cc::string_view const known[] = {"when", "unchecked"};
            judge_attributes(file, attributes, known, "a case");
            auto node
                = case_node{.info = {.name = cc::string(name), .case_declaration = member, .parent = parent}, .decl = c};
            if (auto const* const when = find_attribute(file, attributes, "when"))
            {
                auto const arguments = ast.at(when->arguments);
                if (arguments.size() != 1 || !arguments[0].name.empty() || arguments[0].is_splat)
                    report(diagnostic_kind::invalid_attribute_arguments, file, when->name,
                           "@when takes the condition a value of this case meets: `@when(kind == .glass)`");
                else
                    node.when = arguments[0].value;
            }
            node.info.is_unchecked = find_attribute(file, attributes, "unchecked") != nullptr;
            auto const index = i32(cases.size());
            cases.push_back(cc::move(node));
            if (holds_cases(ast, c->members))
                self(self, c->members, index);
            else
                cases[index].info.leaf = leaf_count++;
        }
    };
    walk(walk, s.members, -1);
    if (leaf_count > 32)
    {
        unsupported(file, s.name, cc::format("a struct of {} cases, more than the 32 one may have yet", leaf_count));
        return;
    }
    // each case's leaves, the leaves below it: every case stands before the cases it holds
    for (auto i = cases.size() - 1; i >= 0; --i)
    {
        if (cases[i].info.leaf >= 0)
            cases[i].info.leaves |= leaf_bit(cases[i].info.leaf);
        if (cases[i].info.parent >= 0)
            cases[cases[i].info.parent].info.leaves |= cases[i].info.leaves;
    }

    // ---- the variant fields --------------------------------------------------------------------------------------------
    auto const commons = cc::vector<member_info>::create_copy_of(out.at(out.at(type).members));
    auto variants = cc::vector<member_info>();
    auto const read_variant = [&](ast::field_id field, cc::string name, u32 leaves, i32 parent)
    {
        auto const& f = ast.at(field);
        cc::string_view const known[] = {"cases"};
        judge_attributes(file, f.attributes,
                         parent < 0 ? cc::span<cc::string_view const>(known) : cc::span<cc::string_view const>(),
                         "a field of a case");
        auto const where = f.name.empty() ? span_of(file, f.type) : f.name;
        // CHK-543: a field one value of a case may hold is no other field of that value
        for (auto const& m : commons)
            if (m.name == name)
            {
                report(diagnostic_kind::duplicate_declaration, file, where, name);
                return;
            }
        for (auto const& m : variants)
            if (m.name == name && (m.leaves & leaves) != 0)
            {
                report(diagnostic_kind::duplicate_declaration, file, where, name);
                return;
            }
        if (f.is_mut)
            unsupported(file, where, "a mut member");
        auto field_type = error_type;
        if (ast::is_valid(f.type))
            field_type = resolve_value_type(file, f.type);
        else
            report(diagnostic_kind::missing_type, file, where, name);
        auto info
            = member_info{.name = cc::move(name), .type = field_type, .field = field, .leaves = leaves, .parent = parent};
        read_field_range(file, f, info);
        variants.push_back(cc::move(info));
    };
    // CHK-548: a root field `@cases` shares stands in the cases it names, and in no other
    for (auto const member : ast.at(s.members))
    {
        auto const* const fd = ast.at(member).node.try_as<ast::field_decl>();
        if (fd == nullptr || !ast::is_valid(fd->field))
            continue;
        auto const* const shared = find_attribute(file, ast.at(fd->field).attributes, "cases");
        if (shared == nullptr)
            continue;
        auto leaves = u32(0);
        auto is_valid_list = ast.at(shared->arguments).size() > 0;
        for (auto const& argument : ast.at(shared->arguments))
        {
            auto const* const dot = ast::is_valid(argument.value) && argument.name.empty()
                                      ? ast.at(argument.value).node.try_as<ast::leading_dot>()
                                      : nullptr;
            auto found = isize(-1);
            if (dot != nullptr)
                for (auto i = isize(0); i < cases.size(); ++i)
                    if (cases[i].info.name == text_of(file, dot->name))
                        found = i;
            if (found < 0)
            {
                is_valid_list = false;
                continue;
            }
            leaves |= cases[found].info.leaves;
        }
        if (!is_valid_list)
        {
            report(diagnostic_kind::invalid_attribute_arguments, file, shared->name,
                   "@cases names the cases that hold the field, each by a leading dot: `@cases(.glass, .metal)`");
            continue;
        }
        read_variant(fd->field, cc::string(text_of(file, ast.at(fd->field).name)), leaves, -1);
    }
    for (auto i = isize(0); i < cases.size(); ++i)
    {
        auto const& c = *cases[i].decl;
        auto const leaves = cases[i].info.leaves;
        for (auto k = u32(0); k < c.payload.count; ++k)
        {
            // a positional field is named by its place, `.0`, as a tuple's is
            auto const field = ast::field_id(i32(c.payload.first + k));
            auto const& f = ast.at(field);
            auto const name = f.name.empty() ? cc::format("{}", k) : cc::string(text_of(file, f.name));
            read_variant(field, name, leaves, i32(i));
        }
        for (auto const member : ast.at(c.members))
        {
            auto const& md = ast.at(member);
            if (auto const* const fd = md.node.try_as<ast::field_decl>(); fd != nullptr && ast::is_valid(fd->field))
            {
                if (ast.at(fd->field).name.empty())
                    continue;
                read_variant(fd->field, cc::string(text_of(file, ast.at(fd->field).name)), leaves, i32(i));
                // CHK-545: the paren fields are positional, so the block's are named
                if (c.has_payload)
                    variants.back().is_named_only_in_case = true;
            }
            else if (!md.node.is<ast::struct_case_decl>() && !md.node.is<ast::invalid_decl>())
                unsupported(file, span_of(file, member), "a member of a case other than a field or a case");
        }
    }

    // ---- the words each variant field takes, first fit over the leaves that hold it ---------------------------------
    auto occupied = cc::vector<cc::vector<u8>>();
    for (auto l = 0; l < leaf_count; ++l)
        occupied.push_back({});
    auto word_count = 0;
    for (auto& v : variants)
    {
        auto const words = words_of(v.type);
        if (words < 0)
        {
            if (v.type != error_type)
                unsupported(file, span_of(file, ast.at(v.field).type),
                            cc::format("a {} in a field of a case, which the variant region holds no words of yet",
                                       out.name_of(v.type)));
            v.type = error_type;
            continue;
        }
        auto const fits = [&](i32 at)
        {
            for (auto l = 0; l < leaf_count; ++l)
                if ((v.leaves & leaf_bit(l)) != 0)
                    for (auto w = at; w < at + words; ++w)
                        if (w < occupied[l].size() && occupied[l][w] != 0)
                            return false;
            return true;
        };
        auto at = 0;
        while (!fits(at))
            ++at;
        for (auto l = 0; l < leaf_count; ++l)
            if ((v.leaves & leaf_bit(l)) != 0)
            {
                if (occupied[l].size() < at + words)
                    occupied[l].resize_to_filled(at + words, u8(0));
                for (auto w = at; w < at + words; ++w)
                    occupied[l][w] = 1;
            }
        v.word = at;
        v.word_count = words;
        word_count = cc::max(word_count, at + words);
    }

    // ---- the discriminator -----------------------------------------------------------------------------------------
    auto const* const tag = find_attribute(file, d.attributes, "tag");
    auto is_external = false;
    if (tag != nullptr)
    {
        auto const arguments = ast.at(tag->arguments);
        auto const* const dot = arguments.size() == 1 && arguments[0].name.empty() && ast::is_valid(arguments[0].value)
                                  ? ast.at(arguments[0].value).node.try_as<ast::leading_dot>()
                                  : nullptr;
        if (dot == nullptr || text_of(file, dot->name) != "external")
            report(diagnostic_kind::invalid_attribute_arguments, file, tag->name,
                   "@tag says where a value's case is held, and `@tag(.external)` that no value holds it");
        else
            is_external = true;
    }
    // CHK-546: one level's cases are told apart by their `@when`s, or by the compiler's tag; never by both
    auto needs_tag = false;
    for (auto parent = isize(-1); parent < cases.size(); ++parent)
    {
        auto count = 0;
        auto with_when = 0;
        for (auto const& c : cases)
            if (c.info.parent == parent)
            {
                ++count;
                with_when += ast::is_valid(c.when) ? 1 : 0;
            }
        if (count == 0)
            continue;
        if (with_when != 0 && with_when != count)
        {
            for (auto const& c : cases)
                if (c.info.parent == parent && !ast::is_valid(c.when))
                    report(diagnostic_kind::invalid_attribute_arguments, file, c.decl->name,
                           cc::format("{} has no @when beside cases of its level that do, and every case of one level "
                                      "says when a value is in it, or none does",
                                      c.info.name));
            continue;
        }
        needs_tag = needs_tag || (with_when == 0 && count > 1);
        if (is_external && with_when != 0)
            report(diagnostic_kind::invalid_attribute_arguments, file, tag->name,
                   "@tag(.external) says no value holds its case, and a @when reads it from the value");
    }
    needs_tag = needs_tag && !is_external;

    // ---- the @when conditions ----------------------------------------------------------------------------------------
    auto members = commons;
    auto is_covered = !is_external;
    for (auto i = isize(0); i < cases.size(); ++i)
    {
        if (!ast::is_valid(cases[i].when))
            continue;
        // a condition reads the root's fields and those of the cases above it, as a default reads the fields before it
        auto readable = cc::vector<readable_field>();
        for (auto k = isize(0); k < commons.size(); ++k)
            readable.push_back({.name = commons[k].name,
                                .type = commons[k].type,
                                .index = i32(k),
                                .has_range = commons[k].has_range,
                                .range_first = commons[k].range_first,
                                .range_last = commons[k].range_last});
        for (auto k = isize(0); k < variants.size(); ++k)
            for (auto up = cases[i].info.parent; up >= 0; up = cases[up].info.parent)
                if (variants[k].parent == up)
                    readable.push_back({.name = variants[k].name,
                                        .type = variants[k].type,
                                        .index = i32(k),
                                        .is_variant = true,
                                        .has_range = variants[k].has_range,
                                        .range_first = variants[k].range_first,
                                        .range_last = variants[k].range_last});
        auto scope = function_scope{.file = file, .result = bool_type};
        for (auto const& r : readable)
        {
            auto const field = r.is_variant ? variants[r.index].field : commons[r.index].field;
            scope.locals.push_back(
                {.name = r.name, .where = {.kind = target_kind::parameter, .index = i32(field)}, .type = r.type});
        }
        (void)check_expected(scope, cases[i].when, bool_type, cc::format("the @when of {}", cases[i].info.name));

        // CHK-546: a field a condition reads tells the cases apart, so nothing assigns it through the struct
        auto reads = cc::vector<cc::string_view>();
        if (!when_names(file, cases[i].when, reads))
        {
            if (!cases[i].info.is_unchecked)
                report(diagnostic_kind::invalid_attribute_arguments, file, span_of(file, cases[i].when),
                       cc::format("this @when is more than comparisons of its fields against constants, joined by "
                                  "`and`, `or` and `not`, so nothing checks it; `@unchecked` takes it as it is"));
            is_covered = false;
        }
        for (auto const& r : readable)
            for (auto const name : reads)
                if (r.name == name)
                    (r.is_variant ? variants[r.index] : members[r.index]).is_discriminant = true;
    }
    for (auto parent = isize(-1); parent < cases.size(); ++parent)
        if (!judge_when_level(file, cc::span<case_node const>(cases), parent, cc::span<member_info const>(commons),
                              cc::span<member_info const>(variants)))
            is_covered = false;
    // CHK-546: a case is built where its path writes its discriminating fields, a value each
    for (auto& c : cases)
    {
        for (auto up = i32(&c - &cases[0]); up >= 0; up = cases[up].info.parent)
            if (ast::is_valid(cases[up].when))
            {
                auto writes = cc::vector<cc::pair<cc::string_view, ast::expr_id>>();
                c.info.is_buildable = c.info.is_buildable && equalities_of(file, cases[up].when, writes);
            }
    }

    // ---- what a host tests a value by, and what its constructor of each leaf writes ------------------------------
    {
        auto const uint_type = prelude_type(builtins::k_uint);
        auto const constant_text = [&](f64 v, type_id of) -> cc::string
        {
            if (of == uint_type)
                return cc::format("{}u", i64(v));
            if (out.name_of(of) == "float" || out.name_of(of) == "half")
                return cc::format("{}f", v);
            return cc::format("{}", i64(v));
        };
        auto const field_of = [&](isize i, cc::string_view name) -> member_info const*
        {
            for (auto const& m : commons)
                if (m.name == name)
                    return &m;
            for (auto up = cases[i].info.parent; up >= 0; up = cases[up].info.parent)
                for (auto const& v : variants)
                    if (v.parent == up && v.name == name)
                        return &v;
            return nullptr;
        };
        auto const reads_of = [&](isize /*i*/, member_info const& m) -> cc::string
        { return m.word >= 0 ? cc::format("{}()", m.name) : m.name; };
        auto const as_c = [&](auto const& self, isize i, ast::expr_id e) -> cc::optional<cc::string>
        {
            auto const& node = ast.at(e).node;
            if (auto const* const n = node.try_as<ast::name>())
            {
                auto const* const f = field_of(i, text_of(file, n->where));
                return f == nullptr ? cc::optional<cc::string>() : cc::optional<cc::string>(reads_of(i, *f));
            }
            auto const* const call = node.try_as<ast::call>();
            if (call == nullptr || !sgl::is_valid(call->op))
                return {};
            auto const op = text_of(file, file_of(file).at(call->op).where);
            auto const arguments = ast.at(call->arguments);
            if (call->is_short_circuit && arguments.size() == 2)
            {
                auto const a = self(self, i, arguments[0].value);
                auto const b = self(self, i, arguments[1].value);
                if (!a.has_value() || !b.has_value())
                    return {};
                return cc::format("({} {} {})", a.value(), op == "and" ? "&&" : "||", b.value());
            }
            if (call->spelling == ast::call_spelling::prefix && arguments.size() == 1)
            {
                auto const a = self(self, i, arguments[0].value);
                return a.has_value() ? cc::optional<cc::string>(cc::format("!{}", a.value()))
                                     : cc::optional<cc::string>();
            }
            if (arguments.size() != 2)
                return {};
            for (auto side = 0; side < 2; ++side)
                if (auto const* const n = ast.at(arguments[side].value).node.try_as<ast::name>())
                {
                    auto const* const f = field_of(i, text_of(file, n->where));
                    auto const c = when_constant(file, arguments[1 - side].value);
                    if (f == nullptr || !c.has_value())
                        return {};
                    auto const constant = constant_text(c.value(), f->type);
                    return side == 0 ? cc::format("{} {} {}", reads_of(i, *f), op, constant)
                                     : cc::format("{} {} {}", constant, op, reads_of(i, *f));
                }
            return {};
        };
        for (auto i = isize(0); i < cases.size(); ++i)
        {
            auto path = cc::vector<isize>();
            for (auto up = i; up >= 0; up = cases[up].info.parent)
                path.insert_at(0, up);
            auto parts = cc::vector<cc::string>();
            auto is_told = !is_external;
            auto tagged = isize(-1);
            for (auto const j : path)
            {
                if (!ast::is_valid(cases[j].when))
                {
                    tagged = j;
                    continue;
                }
                auto is_ordered = false;
                for (auto const& k : cases)
                    is_ordered = is_ordered || (k.info.parent == cases[j].info.parent && k.info.is_unchecked);
                auto const rendered = is_ordered ? cc::optional<cc::string>() : as_c(as_c, j, cases[j].when);
                is_told = is_told && rendered.has_value();
                if (rendered.has_value())
                    parts.push_back(rendered.value());
            }
            if (tagged >= 0 && needs_tag)
            {
                auto lo = 32;
                auto hi = -1;
                for (auto l = 0; l < leaf_count; ++l)
                    if ((cases[tagged].info.leaves & leaf_bit(l)) != 0)
                    {
                        lo = cc::min(lo, l);
                        hi = cc::max(hi, l);
                    }
                parts.insert_at(
                    0, lo == hi ? cc::format("_tag == {}u", lo) : cc::format("(_tag >= {}u && _tag <= {}u)", lo, hi));
            }
            if (!is_told)
                continue;
            auto condition = cc::string();
            for (auto const& part : parts)
                condition.appendf("{}{}", condition.empty() ? "" : " && ", part);
            cases[i].info.host_condition = condition.empty() ? cc::string("true") : condition;
        }
        for (auto i = isize(0); i < cases.size(); ++i)
        {
            if (cases[i].info.leaf < 0 || !cases[i].info.is_buildable)
                continue;
            auto& writes = cases[i].info.host_writes;
            auto const add = [&](cc::string_view name, cc::string_view value)
            { writes.appendf("{}{}={}", writes.empty() ? "" : ";", name, value); };
            if (needs_tag)
                add("_tag", cc::format("{}u", cases[i].info.leaf));
            for (auto up = i32(i); up >= 0; up = cases[up].info.parent)
            {
                if (!ast::is_valid(cases[up].when))
                    continue;
                auto fixed = cc::vector<cc::pair<cc::string_view, ast::expr_id>>();
                (void)equalities_of(file, cases[up].when, fixed);
                for (auto const& [name, e] : fixed)
                {
                    auto const* const f = field_of(up, name);
                    if (f == nullptr)
                        continue;
                    auto value = cc::string();
                    if (ast.at(e).node.is<ast::name>())
                        value = "1";
                    else if (auto const* const call = ast.at(e).node.try_as<ast::call>();
                             call != nullptr && call->spelling == ast::call_spelling::prefix)
                        value = "0";
                    else if (auto const c = when_constant(file, e); c.has_value())
                        value = constant_text(c.value(), f->type);
                    add(name, value);
                }
            }
        }
    }

    // ---- the carrier: the tag in the first hole the common fields leave, then the words ----------------------------
    auto tag_member = -1;
    if (needs_tag)
    {
        auto at = 0;
        auto hole = members.size();
        for (auto k = isize(0); k < members.size(); ++k)
        {
            auto const placed = natural_layout_of(members[k].type);
            auto const start = round_up(at, placed.alignment);
            if (start - at >= 4 && at % 4 == 0)
            {
                hole = k;
                break;
            }
            at = start + placed.size;
        }
        tag_member = i32(hole);
        members.insert_at(hole, member_info{.name = "_tag", .type = uint_type, .is_hidden = true});
    }
    auto const first_word = word_count > 0 ? i32(members.size()) : -1;
    for (auto w = 0; w < word_count; ++w)
        members.push_back(member_info{.name = cc::format("_w{}", w), .type = uint_type, .is_hidden = true});
    for (auto const& m : commons)
        if (m.name == "_tag" || (m.name.size() > 2 && m.name.starts_with("_w")))
            report(diagnostic_kind::duplicate_declaration, file, ast.at(m.field).name,
                   cc::format("{} is a name a struct with cases gives its own layout", m.name));

    auto case_infos = cc::vector<member_info>();
    for (auto& c : cases)
        case_infos.push_back(cc::move(c.info));
    auto& t = out.types.own(index_of(type));
    t.members = {.first = u32(out.members.size()), .count = u32(members.size())};
    out.members.push_back_range(members);
    t.struct_cases = {.first = u32(out.members.size()), .count = u32(case_infos.size())};
    out.members.push_back_range(case_infos);
    t.variant_fields = {.first = u32(out.members.size()), .count = u32(variants.size())};
    out.members.push_back_range(variants);
    t.leaf_count = leaf_count;
    t.tag_member = tag_member;
    t.first_word = first_word;
    t.word_count = word_count;
    t.is_external_tag = is_external;
    t.is_covered = is_covered;
}

void checker::read_field_range(i32 file, ast::field const& f, member_info& into)
{
    if (!ast::is_valid(f.range))
        return;
    auto const& ast = ast_of(file);
    auto const* const r = ast.at(f.range).node.try_as<ast::range>();
    auto const where = span_of(file, f.range);
    auto const is_integer = into.type == prelude_type(builtins::k_int) || into.type == prelude_type(builtins::k_uint);
    if (into.type == error_type)
        return;
    if (!is_integer)
    {
        unsupported(file, where, cc::format("a range of {}; an int or a uint field takes one", out.name_of(into.type)));
        return;
    }
    auto const first = r != nullptr ? number_of(file, r->first) : number_literal();
    auto const last = r != nullptr ? number_of(file, r->last) : number_literal();
    if (r == nullptr || !first.is_number || !first.is_integer || !last.is_number || !last.is_integer)
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, where,
               "a field's range is two integer literals, `x: int in 0..<10`");
        return;
    }
    auto const is_inclusive = text_of(file, file_of(file).at(r->op).where) == "..=";
    auto const lowest = into.type == prelude_type(builtins::k_uint) ? i64(0) : i64(-2147483647 - 1);
    auto const highest = into.type == prelude_type(builtins::k_uint) ? i64(4294967295) : i64(2147483647);
    auto const last_value = is_inclusive ? last.integer : last.integer - 1;
    if (first.integer > last_value || first.integer < lowest || last_value > highest)
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, where,
               cc::format("{} holds no value of this range", out.name_of(into.type)));
        return;
    }
    into.has_range = true;
    into.range_first = first.integer;
    into.range_last = last_value;
}

i32 checker::words_of(type_id type) const
{
    if (type == error_type)
        return -1;
    if (auto const* const record = out.builtin_type_of(type))
    {
        auto const& t = out.at(type);
        if (t.kind == type_kind::enumeration)
            return 1;
        auto const bytes = scalar_bytes_of(record->leaf_kind);
        // a scalar or a vector, each lane a word; a matrix and a 64-bit value hold none yet
        auto const is_lanes = record->leaf_count == 1 || t.has_swizzles;
        if (bytes == 0 || bytes > 4 || !is_lanes)
            return -1;
        return record->leaf_count;
    }
    auto const& t = out.at(type);
    if (t.kind == type_kind::enumeration)
        return 1;
    if (t.kind == type_kind::array)
    {
        auto const element = words_of(t.element);
        return element < 0 || t.count <= 0 ? -1 : element * t.count;
    }
    if (t.kind != type_kind::structure || t.is_opaque || t.has_target_layout || t.is_text_type)
        return -1;
    auto total = 0;
    for (auto const& m : out.at(t.members))
    {
        auto const words = words_of(m.type);
        if (words < 0)
            return -1;
        total += words;
    }
    return total;
}

checker::natural_layout checker::natural_layout_of(type_id type) const
{
    if (type == error_type)
        return {.size = 4, .alignment = 4};
    if (auto const* const record = out.builtin_type_of(type))
        return {.size = record->wgsl_layout.size, .alignment = cc::max(record->wgsl_layout.alignment, 1)};
    auto const& t = out.at(type);
    if (t.kind == type_kind::enumeration)
        return {.size = 4, .alignment = 4};
    if (t.kind == type_kind::array)
    {
        auto const element = natural_layout_of(t.element);
        return {.size = round_up(element.size, element.alignment) * cc::max(t.count, 1), .alignment = element.alignment};
    }
    auto at = 0;
    auto alignment = 1;
    for (auto const& m : out.at(t.members))
    {
        auto const placed = natural_layout_of(m.type);
        at = round_up(at, placed.alignment) + placed.size;
        alignment = cc::max(alignment, placed.alignment);
    }
    return {.size = round_up(at, alignment), .alignment = alignment};
}

// ---- the @when fragment ---------------------------------------------------------------------------------------------

bool checker::when_names(i32 file, ast::expr_id expr, cc::vector<cc::string_view>& names) const
{
    auto const& ast = ast_of(file);
    if (!ast::is_valid(expr))
        return false;
    auto const& node = ast.at(expr).node;
    if (auto const* const n = node.try_as<ast::name>())
    {
        auto const& where = tables_of(file).target_at(expr);
        if (where.kind != target_kind::parameter)
            return false;
        names.push_back(text_of(file, n->where));
        return true;
    }
    auto const* const call = node.try_as<ast::call>();
    if (call == nullptr || !sgl::is_valid(call->op))
        return false;
    auto const op = text_of(file, file_of(file).at(call->op).where);
    auto const arguments = ast.at(call->arguments);
    if (call->is_short_circuit && (op == "and" || op == "or") && arguments.size() == 2)
        return when_names(file, arguments[0].value, names) && when_names(file, arguments[1].value, names);
    if (call->spelling == ast::call_spelling::prefix && op == "not" && arguments.size() == 1)
        return when_names(file, arguments[0].value, names);
    auto const is_comparison = op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
    if (!is_comparison || arguments.size() != 2)
        return false;
    // a field on one side, a constant on the other
    for (auto side = 0; side < 2; ++side)
    {
        auto const* const field = ast.at(arguments[side].value).node.try_as<ast::name>();
        if (field == nullptr || tables_of(file).target_at(arguments[side].value).kind != target_kind::parameter)
            continue;
        if (!when_constant(file, arguments[1 - side].value).has_value())
            return false;
        names.push_back(text_of(file, field->where));
        return true;
    }
    return false;
}

cc::optional<f64> checker::when_constant(i32 file, ast::expr_id expr) const
{
    if (!ast::is_valid(expr))
        return {};
    auto const& where = tables_of(file).target_at(expr);
    if (where.kind == target_kind::enum_case && is_valid(where.symbol))
    {
        auto const type = out.at(where.symbol).type;
        if (!is_valid(type) || out.at(type).kind != type_kind::enumeration)
            return {};
        // `bool`'s cases are its values
        if (out.builtin_type_of(type) != nullptr)
            return f64(where.index);
        return f64(out.at(out.at(type).cases)[where.index].value);
    }
    if (where.kind == target_kind::symbol && is_valid(where.symbol)
        && out.at(where.symbol).kind == symbol_kind::constant && out.at(where.symbol).info >= 0)
    {
        auto const& c = out.constants[out.at(where.symbol).info];
        if (c.kind == constant_kind::integer)
            return c.is_unsigned ? f64(u32(c.integer)) : f64(c.integer);
        if (c.kind == constant_kind::real)
            return c.real;
        if (c.kind == constant_kind::enum_case)
        {
            if (out.builtin_type_of(c.type) != nullptr)
                return f64(c.case_index);
            return f64(out.at(out.at(c.type).cases)[c.case_index].value);
        }
        return {};
    }
    auto const number = number_of(file, expr);
    if (number.is_number && !number.is_past_long)
        return number.is_integer ? f64(number.integer) : number.real;
    return {};
}

cc::optional<bool> checker::when_holds(i32 file,
                                       ast::expr_id expr,
                                       cc::span<cc::string_view const> fields,
                                       cc::span<f64 const> values) const
{
    auto const& ast = ast_of(file);
    auto const value_of = [&](ast::expr_id e) -> cc::optional<f64>
    {
        if (auto const* const n = ast.at(e).node.try_as<ast::name>();
            n != nullptr && tables_of(file).target_at(e).kind == target_kind::parameter)
            for (auto i = isize(0); i < fields.size(); ++i)
                if (fields[i] == text_of(file, n->where))
                    return values[i];
        return when_constant(file, e);
    };
    auto const& node = ast.at(expr).node;
    if (node.is<ast::name>())
    {
        auto const v = value_of(expr);
        return v.has_value() ? cc::optional<bool>(v.value() != 0) : cc::optional<bool>();
    }
    auto const* const call = node.try_as<ast::call>();
    if (call == nullptr || !sgl::is_valid(call->op))
        return {};
    auto const op = text_of(file, file_of(file).at(call->op).where);
    auto const arguments = ast.at(call->arguments);
    if (call->is_short_circuit && arguments.size() == 2)
    {
        auto const a = when_holds(file, arguments[0].value, fields, values);
        auto const b = when_holds(file, arguments[1].value, fields, values);
        if (!a.has_value() || !b.has_value())
            return {};
        return op == "and" ? (a.value() && b.value()) : (a.value() || b.value());
    }
    if (call->spelling == ast::call_spelling::prefix && arguments.size() == 1)
    {
        auto const a = when_holds(file, arguments[0].value, fields, values);
        return a.has_value() ? cc::optional<bool>(!a.value()) : cc::optional<bool>();
    }
    if (arguments.size() != 2)
        return {};
    auto const a = value_of(arguments[0].value);
    auto const b = value_of(arguments[1].value);
    if (!a.has_value() || !b.has_value())
        return {};
    if (op == "==")
        return a.value() == b.value();
    if (op == "!=")
        return a.value() != b.value();
    if (op == "<")
        return a.value() < b.value();
    if (op == "<=")
        return a.value() <= b.value();
    if (op == ">")
        return a.value() > b.value();
    return a.value() >= b.value();
}

bool checker::equalities_of(i32 file, ast::expr_id expr, cc::vector<cc::pair<cc::string_view, ast::expr_id>>& writes) const
{
    auto const& ast = ast_of(file);
    auto const& node = ast.at(expr).node;
    // a bare bool field is `field == true`, and its `not` is `field == false`; the value is the expression itself
    if (auto const* const n = node.try_as<ast::name>())
    {
        writes.push_back({text_of(file, n->where), expr});
        return tables_of(file).target_at(expr).kind == target_kind::parameter;
    }
    auto const* const call = node.try_as<ast::call>();
    if (call == nullptr || !sgl::is_valid(call->op))
        return false;
    auto const op = text_of(file, file_of(file).at(call->op).where);
    auto const arguments = ast.at(call->arguments);
    if (call->is_short_circuit && op == "and" && arguments.size() == 2)
        return equalities_of(file, arguments[0].value, writes) && equalities_of(file, arguments[1].value, writes);
    if (call->spelling == ast::call_spelling::prefix && op == "not" && arguments.size() == 1)
    {
        auto const* const n = ast.at(arguments[0].value).node.try_as<ast::name>();
        if (n == nullptr || tables_of(file).target_at(arguments[0].value).kind != target_kind::parameter)
            return false;
        writes.push_back({text_of(file, n->where), expr});
        return true;
    }
    if (op != "==" || arguments.size() != 2)
        return false;
    for (auto side = 0; side < 2; ++side)
        if (auto const* const n = ast.at(arguments[side].value).node.try_as<ast::name>();
            n != nullptr && tables_of(file).target_at(arguments[side].value).kind == target_kind::parameter
            && when_constant(file, arguments[1 - side].value).has_value())
        {
            auto const name = text_of(file, n->where);
            // two values of one field: no value is in this case at all, and nothing builds it
            for (auto const& w : writes)
                if (w.first == name)
                    return false;
            writes.push_back({name, arguments[1 - side].value});
            return true;
        }
    return false;
}

template <class Case>
bool checker::judge_when_level(i32 file,
                               cc::span<Case const> cases,
                               isize parent,
                               cc::span<member_info const> commons,
                               cc::span<member_info const> variants)
{
    // the cases of the level, and the conditions on the way down to it
    auto siblings = cc::vector<isize>();
    for (auto i = isize(0); i < cases.size(); ++i)
        if (cases[i].info.parent == parent)
            siblings.push_back(i);
    if (siblings.empty() || !ast::is_valid(cases[siblings[0]].when))
        return true;
    auto conditions = cc::vector<ast::expr_id>();
    for (auto up = parent; up >= 0; up = cases[up].info.parent)
        if (ast::is_valid(cases[up].when))
            conditions.push_back(cases[up].when);
    for (auto const s : siblings)
        if (cases[s].info.is_unchecked)
            return false;

    // the fields they read, and values of each that stand for every region the conditions tell apart
    auto names = cc::vector<cc::string_view>();
    auto all = conditions;
    for (auto const s : siblings)
        all.push_back(cases[s].when);
    for (auto const c : all)
        if (!when_names(file, c, names))
            return false;
    auto fields = cc::vector<cc::string_view>();
    for (auto const n : names)
    {
        auto seen = false;
        for (auto const f : fields)
            seen = seen || f == n;
        if (!seen)
            fields.push_back(n);
    }
    auto domains = cc::vector<cc::vector<f64>>();
    for (auto const name : fields)
    {
        auto const* field = static_cast<member_info const*>(nullptr);
        for (auto const& m : commons)
            if (m.name == name)
                field = &m;
        for (auto const& m : variants)
            if (m.name == name && field == nullptr)
                field = &m;
        if (field == nullptr)
            return false;
        auto values = cc::vector<f64>();
        auto const& t = out.at(field->type);
        if (field->type == type_of_builtin(builtins::k_bool, file, {}))
            values = {0.0, 1.0};
        else if (t.kind == type_kind::enumeration)
            for (auto const& c : out.at(t.cases))
                values.push_back(f64(c.value));
        else if (out.name_of(field->type) == "float" || out.name_of(field->type) == "half")
        {
            // each constant, a value between each two, and one past either end stand for every region a comparison
            // tells apart; a NaN is none of them, as the default float level assumes none arises (CHK-435)
            auto constants = cc::vector<f64>();
            auto const collect = [&](auto const& self, ast::expr_id e) -> void
            {
                if (auto const* const call = ast_of(file).at(e).node.try_as<ast::call>())
                    for (auto const& a : ast_of(file).at(call->arguments))
                    {
                        if (auto const c = when_constant(file, a.value); c.has_value())
                            constants.push_back(c.value());
                        self(self, a.value);
                    }
            };
            for (auto const c : all)
                collect(collect, c);
            for (auto i = isize(0); i < constants.size(); ++i)
                for (auto k = i + 1; k < constants.size(); ++k)
                    if (constants[k] < constants[i])
                        cc::swap(constants[i], constants[k]);
            for (auto i = isize(0); i < constants.size(); ++i)
            {
                values.push_back(constants[i]);
                if (i + 1 < constants.size())
                    values.push_back((constants[i] + constants[i + 1]) * 0.5);
            }
            values.push_back(constants.empty() ? 0.0 : constants.front() - 1.0);
            values.push_back(constants.empty() ? 0.0 : constants.back() + 1.0);
        }
        else
        {
            auto const is_uint = field->type == prelude_type(builtins::k_uint);
            auto const lowest = f64(field->has_range ? field->range_first : is_uint ? i64(0) : i64(-2147483647 - 1));
            auto const highest = f64(field->has_range ? field->range_last
                                     : is_uint        ? i64(4294967295)
                                                      : i64(2147483647));
            values = {lowest, highest};
            // every constant a condition compares the field with, and its neighbours, stand for each region between
            auto const collect = [&](auto const& self, ast::expr_id e) -> void
            {
                auto const& node = ast_of(file).at(e).node;
                if (auto const* const call = node.try_as<ast::call>())
                    for (auto const& a : ast_of(file).at(call->arguments))
                    {
                        if (auto const c = when_constant(file, a.value); c.has_value())
                            for (auto const v : {c.value() - 1.0, c.value(), c.value() + 1.0})
                                if (v >= lowest && v <= highest)
                                    values.push_back(v);
                        self(self, a.value);
                    }
            };
            for (auto const c : all)
                collect(collect, c);
        }
        domains.push_back(cc::move(values));
    }
    auto total = i64(1);
    for (auto const& dmn : domains)
        total *= i64(dmn.size());
    if (total > 4096)
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, cases[siblings[0]].decl->name,
               "the @whens of this level read too many values to check; `@unchecked` takes them as they are");
        return false;
    }

    auto covers = true;
    auto overlap_reported = false;
    auto values = cc::vector<f64>();
    for (auto f = isize(0); f < fields.size(); ++f)
        values.push_back(0.0);
    for (auto n = i64(0); n < total; ++n)
    {
        auto rest = n;
        for (auto f = isize(0); f < fields.size(); ++f)
        {
            values[f] = domains[f][rest % domains[f].size()];
            rest /= domains[f].size();
        }
        auto is_inside = true;
        for (auto const c : conditions)
            is_inside = is_inside && when_holds(file, c, fields, values).value_or(false);
        if (!is_inside)
            continue;
        auto matched = cc::vector<isize>();
        for (auto const s : siblings)
            if (when_holds(file, cases[s].when, fields, values).value_or(false))
                matched.push_back(s);
        if (matched.empty())
            covers = false;
        if (matched.size() > 1 && !overlap_reported)
        {
            overlap_reported = true;
            auto at = cc::string();
            for (auto f = isize(0); f < fields.size(); ++f)
                at.appendf("{}{} = {}", f == 0 ? "" : ", ", fields[f], values[f]);
            report(diagnostic_kind::overlapping_cases, file, cases[matched[1]].decl->name,
                   cc::format("a value with {} is in {} and in {}", at, cases[matched[0]].info.name,
                              cases[matched[1]].info.name));
        }
    }
    return covers && !overlap_reported;
}

// ---- constructors -----------------------------------------------------------------------------------------------------

void checker::compile_case_constructor(symbol_id id)
{
    auto const structure = out.at(id).owner;
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const where = ast.at(out.at(id).declaration).node.as<ast::struct_case_decl>().name;
    auto const is_checked = demand(structure, file, where) == symbol_state::checked;

    auto parameters = cc::vector<parameter>();
    auto result = error_type;
    auto case_index = -1;
    if (is_checked)
    {
        result = out.at(structure).type;
        auto const& t = out.at(result);
        auto const cases = out.at(t.struct_cases);
        for (auto i = isize(0); i < cases.size(); ++i)
            if (cases[i].case_declaration == out.at(id).declaration)
                case_index = i32(i);
    }
    if (case_index >= 0)
    {
        auto const& t = out.at(result);
        auto const cases = out.at(t.struct_cases);
        auto const& leaf = cases[case_index];
        // the fields a `@when` on the path fixes are the constructor's to write
        auto fixed = cc::vector<cc::pair<cc::string_view, ast::expr_id>>();
        auto path = cc::vector<i32>();
        for (auto up = case_index; up >= 0; up = cases[up].parent)
            path.insert_at(0, up);
        for (auto const up : path)
            if (auto const* const when = find_attribute(file, ast.at(cases[up].case_declaration).attributes, "when");
                when != nullptr && leaf.is_buildable && ast.at(when->arguments).size() == 1)
                (void)equalities_of(file, ast.at(when->arguments)[0].value, fixed);
        auto const is_fixed = [&](cc::string_view name)
        {
            for (auto const& w : fixed)
                if (w.first == name)
                    return true;
            return false;
        };
        auto const add = [&](member_info const& m, bool is_named_only)
        {
            if (m.is_hidden || is_fixed(m.name) || !ast::is_valid(m.field))
                return;
            auto const& f = ast.at(m.field);
            parameters.push_back({.name = m.name,
                                  .type = m.type,
                                  .field = m.field,
                                  .has_default = ast::is_valid(f.default_value),
                                  .is_named_only = is_named_only || f.is_named_only});
        };
        for (auto const& m : out.at(t.members))
            add(m, false);
        auto const variants = out.at(t.variant_fields);
        for (auto const& v : variants)
            if (v.parent < 0 && (v.leaves & leaf.leaves) != 0)
                add(v, false);
        for (auto const up : path)
            for (auto const& v : variants)
                if (v.parent == up)
                    add(v, v.is_named_only_in_case);
    }

    out.symbols.own(index_of(id)).info = i32(out.functions.size());
    out.functions.push_back({
        .symbol = id,
        .parameters = {.first = u32(out.parameters.size()), .count = u32(parameters.size())},
        .result = result,
        .result_cases = case_index >= 0 ? out.at(out.at(result).struct_cases)[case_index].leaves : 0,
        .case_index = case_index,
        .is_pure = true,
    });
    out.parameters.push_back_range(parameters);
    notes.push_back({.is_body_checked = true, .is_body_sound = true});
    if (!is_checked || case_index < 0)
        out.symbols.own(index_of(id)).state = symbol_state::failed;
}

// ---- naming cases -----------------------------------------------------------------------------------------------------

cc::optional<u32> checker::cases_named(i32 file, ast::expr_id pattern, type_id type)
{
    auto const& ast = ast_of(file);
    if (!ast::is_valid(pattern) || type == error_type)
        return {};
    auto const& t = out.at(type);
    auto const& node = ast.at(pattern).node;
    auto const where = span_of(file, pattern);
    auto const find = [&](cc::string_view name) -> cc::optional<u32>
    {
        for (auto const& c : out.at(t.struct_cases))
            if (c.name == name)
                return c.leaves;
        report(diagnostic_kind::unknown_member, file, where, cc::format("{} has no case {}", out.name_of(type), name));
        return {};
    };
    if (auto const* const call = node.try_as<ast::call>(); call != nullptr && call->is_short_circuit
                                                           && sgl::is_valid(call->op)
                                                           && text_of(file, file_of(file).at(call->op).where) == "or")
    {
        auto result = u32(0);
        auto is_named = true;
        for (auto const& a : ast.at(call->arguments))
        {
            auto const one = cases_named(file, a.value, type);
            is_named = is_named && one.has_value();
            result |= one.value_or(0);
        }
        if (!is_named)
            return {};
        set_type(file, pattern, type);
        set_target(file, pattern, {.kind = target_kind::struct_cases, .symbol = t.symbol, .index = i32(result)});
        return result;
    }
    auto found = cc::optional<u32>();
    auto view_type = type_id::none;
    auto view = u32(0);
    if (auto const* const dot = node.try_as<ast::leading_dot>())
        found = find(text_of(file, dot->name));
    else if (resolve_case_view(file, pattern, view_type, view))
    {
        if (view_type != type)
        {
            report(diagnostic_kind::type_mismatch, file, where,
                   cc::format("a case of {} stands against a {}", out.name_of(view_type), out.name_of(type)));
            return {};
        }
        found = view;
    }
    else
    {
        report(diagnostic_kind::unknown_member, file, where,
               cc::format("a case of {} is named by a leading dot, `.glass`", out.name_of(type)));
        return {};
    }
    if (found.has_value())
    {
        set_type(file, pattern, type);
        set_target(file, pattern, {.kind = target_kind::struct_cases, .symbol = t.symbol, .index = i32(found.value())});
    }
    return found;
}

bool checker::resolve_case_view(i32 file, ast::expr_id expr, type_id& type, u32& cases)
{
    auto const& ast = ast_of(file);
    if (!ast::is_valid(expr))
        return false;
    auto const& node = ast.at(expr).node;
    // `a or b`: the cases of either, of one struct
    if (auto const* const call = node.try_as<ast::call>(); call != nullptr && call->is_short_circuit
                                                           && sgl::is_valid(call->op)
                                                           && text_of(file, file_of(file).at(call->op).where) == "or")
    {
        auto const arguments = ast.at(call->arguments);
        if (arguments.size() != 2)
            return false;
        auto left_type = type_id::none;
        auto left = u32(0);
        auto right_type = type_id::none;
        auto right = u32(0);
        if (!resolve_case_view(file, arguments[0].value, left_type, left)
            || !resolve_case_view(file, arguments[1].value, right_type, right))
            return false;
        if (left_type != right_type)
        {
            report(diagnostic_kind::type_mismatch, file, span_of(file, expr),
                   cc::format("{} and {} are cases of two structs, and one type holds the cases of one",
                              out.name_of(left_type), out.name_of(right_type)));
            type = error_type;
            cases = 0;
            return true;
        }
        type = left_type;
        cases = left | right;
        return true;
    }
    auto const* const member = node.try_as<ast::member>();
    if (member == nullptr)
        return false;
    auto const* const found = symbols_named(file, member->object, nullptr);
    if (found == nullptr || found->empty())
        return false;
    auto const owner = declared_type_of(found->front(), file, span_of(file, member->object));
    if (!is_valid(owner) || out.at(owner).kind != symbol_kind::structure)
        return false;
    if (demand(owner, file, span_of(file, member->object)) != symbol_state::checked)
    {
        type = error_type;
        cases = 0;
        return true;
    }
    auto const struct_type = out.at(owner).type;
    if (!out.at(struct_type).has_cases())
        return false;
    auto const name = text_of(file, member->name);
    for (auto const& c : out.at(out.at(struct_type).struct_cases))
        if (c.name == name)
        {
            set_target(file, member->object, {.kind = target_kind::symbol, .symbol = found->front()});
            type = struct_type;
            cases = c.leaves;
            return true;
        }
    return false;
}

// ---- what is known of a value -----------------------------------------------------------------------------------------

u32 checker::cases_of(i32 file, ast::expr_id expr, type_id type) const
{
    if (type == error_type || !out.at(type).has_cases())
        return 0;
    auto const every = out.at(type).every_leaf();
    auto const* const found = expr_cases.get_ptr((u64(u32(file)) << 32) | u64(u32(index_of(expr))));
    return found != nullptr && *found != 0 ? *found & every : every;
}

void checker::set_cases(i32 file, ast::expr_id expr, u32 cases)
{
    if (ast::is_valid(expr) && cases != 0)
        expr_cases[(u64(u32(file)) << 32) | u64(u32(index_of(expr)))] = cases;
}

cc::string checker::case_names_of(type_id type, u32 cases) const
{
    auto result = cc::string();
    for (auto const& c : out.at(out.at(type).struct_cases))
        if (c.leaf >= 0 && (cases & leaf_bit(c.leaf)) != 0)
            result.appendf("{}.{}", result.empty() ? "" : ", ", c.name);
    return result;
}

void checker::judge_cases(i32 file, source_span where, type_id type, u32 have, u32 want)
{
    if (want == 0 || type == error_type || !out.at(type).has_cases())
        return;
    auto const outside = have & ~want;
    if (outside == 0)
        return;
    report(diagnostic_kind::type_mismatch, file, where,
           cc::format("this {} may be {}, which this place does not take; narrow it with `is` first, or state its "
                      "case with `as`",
                      out.name_of(type), case_names_of(type, outside)));
}

// ---- is, narrowing and case --------------------------------------------------------------------------------------------

type_id checker::check_case_test(function_scope& scope, ast::expr_id id, ast::case_test const& node)
{
    auto const file = scope.file;
    auto const bool_type = type_of_builtin(builtins::k_bool, file, {});
    auto const value = check_expr(scope, node.value);
    if (value == error_type)
        return error_type;
    auto const& t = out.at(value);
    if (!t.has_cases())
    {
        report(diagnostic_kind::type_mismatch, file, span_of(file, node.value),
               cc::format("{} is a struct without cases, so `is` asks nothing of it", out.name_of(value)));
        return error_type;
    }
    // CHK-547: a value that holds no case of its own tells none
    if (t.is_external_tag)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, id),
               cc::format("{} is @tag(.external): no value of it says its case, so `is` cannot ask; `as` states it",
                          out.name_of(value)));
        return error_type;
    }
    auto const named = cases_named(file, node.pattern, value);
    if (!named.has_value())
        return error_type;
    auto const tested = node.is_negated ? ~named.value() & t.every_leaf() : named.value();
    set_target(file, id, {.kind = target_kind::struct_cases, .symbol = t.symbol, .index = i32(tested)});
    return bool_type;
}

cc::vector<checker::narrowing> checker::narrowings_of(function_scope& scope, ast::expr_id condition, bool holds)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    if (!ast::is_valid(condition))
        return {};
    auto const& node = ast.at(condition).node;
    if (auto const* const test = node.try_as<ast::case_test>())
    {
        auto const& where = tables_of(file).target_at(condition);
        auto const* const name = ast.at(test->value).node.try_as<ast::name>();
        if (where.kind != target_kind::struct_cases || name == nullptr)
            return {};
        auto const text = text_of(file, name->where);
        auto const* const local = scope.find_local(text);
        // CHK-542: a `mut` name may be assigned between the test and a read, so nothing narrows it
        if (local == nullptr || local->is_mut || local->is_captured || local->type == error_type
            || !out.at(local->type).has_cases())
            return {};
        auto const every = out.at(local->type).every_leaf();
        auto const known = local->cases != 0 ? local->cases : every;
        auto const tested = u32(where.index);
        return {{.name = text, .cases = holds ? known & tested : known & ~tested & every}};
    }
    auto const* const call = node.try_as<ast::call>();
    if (call == nullptr || !sgl::is_valid(call->op))
        return {};
    auto const op = text_of(file, file_of(file).at(call->op).where);
    auto const arguments = ast.at(call->arguments);
    if (call->spelling == ast::call_spelling::prefix && op == "not" && arguments.size() == 1)
        return narrowings_of(scope, arguments[0].value, !holds);
    if (!call->is_short_circuit || arguments.size() != 2 || (op != "and" && op != "or"))
        return {};
    // `a and b` holds where both do, the second seen where the first holds; it fails where either does
    auto const is_and = op == "and";
    auto const left = narrowings_of(scope, arguments[0].value, is_and);
    auto const visible = scope.locals.size();
    narrow(scope, left);
    auto const right = narrowings_of(scope, arguments[1].value, holds);
    scope.locals.resize_down_to(visible);
    if (holds == is_and)
    {
        // both hold: the second's narrowings were made against the first's
        auto result = left;
        for (auto const& r : right)
        {
            auto replaced = false;
            for (auto& l : result)
                if (l.name == r.name)
                {
                    l.cases = r.cases;
                    replaced = true;
                }
            if (!replaced)
                result.push_back(r);
        }
        return result;
    }
    // one of two paths: the first fails alone, or it holds and the second fails
    auto first_only = narrowings_of(scope, arguments[0].value, holds);
    auto second = left;
    for (auto const& r : right)
    {
        auto replaced = false;
        for (auto& l : second)
            if (l.name == r.name)
            {
                l.cases = r.cases;
                replaced = true;
            }
        if (!replaced)
            second.push_back(r);
    }
    cc::vector<narrowing> const paths[] = {first_only, second};
    return join(paths);
}

cc::vector<checker::narrowing> checker::join(cc::span<cc::vector<narrowing> const> paths)
{
    if (paths.empty())
        return {};
    auto result = cc::vector<narrowing>();
    for (auto const& n : paths[0])
    {
        auto joined = n;
        auto is_everywhere = true;
        for (auto const& other : paths.subspan(1))
        {
            auto found = false;
            for (auto const& o : other)
                if (o.name == n.name)
                {
                    joined.cases |= o.cases;
                    found = true;
                }
            is_everywhere = is_everywhere && found;
        }
        if (is_everywhere)
            result.push_back(joined);
    }
    return result;
}

void checker::narrow(function_scope& scope, cc::span<narrowing const> narrowings)
{
    for (auto const& n : narrowings)
        if (auto const* const local = scope.find_local(n.name))
        {
            auto shadow = *local;
            shadow.cases = n.cases;
            shadow.depth = scope.depth;
            scope.locals.push_back(shadow);
        }
}

type_id checker::check_struct_case(function_scope& scope,
                                   ast::expr_id id,
                                   ast::case_expr const& node,
                                   type_id scrutinee,
                                   bool yields_value,
                                   flow* ending,
                                   type_id expected)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, id);
    auto const& t = out.at(scrutinee);
    if (t.is_external_tag)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, node.value),
               cc::format("{} is @tag(.external): no value of it says its case, so no arm can ask; `as` states it",
                          out.name_of(scrutinee)));
        return error_type;
    }
    // the arms narrow a name that is no `mut`, as `is` does
    auto const* const name = ast.at(node.value).node.try_as<ast::name>();
    auto const* const local = name != nullptr ? scope.find_local(text_of(file, name->where)) : nullptr;
    auto const is_narrowed = local != nullptr && !local->is_mut && !local->is_captured;
    auto remaining = cases_of(file, node.value, scrutinee);

    auto result = yields_value ? expected : checked_module::void_type;
    auto is_failed = false;
    auto has_wildcard = false;
    auto every_arm_exits = true;
    auto arm_count = 0;
    auto reported_unreachable = false;
    for (auto const& arm : ast.at(node.arms))
    {
        if (!ast::is_valid(arm.pattern) && arm.result.kind == ast::body_kind::none)
        {
            is_failed = true;
            every_arm_exits = false;
            continue;
        }
        ++arm_count;
        auto selected = remaining;
        if (ast::is_valid(arm.pattern) && ast.at(arm.pattern).node.is<ast::wildcard>())
        {
            has_wildcard = true;
            set_type(file, arm.pattern, scrutinee);
        }
        else if (auto const named = cases_named(file, arm.pattern, scrutinee); named.has_value())
            selected = remaining & named.value();
        else
            is_failed = true;
        // CHK-551: an arm whose cases the arms before it took never runs
        if ((selected == 0 || (has_wildcard && remaining == 0)) && !reported_unreachable && !is_failed)
        {
            reported_unreachable = true;
            report(diagnostic_kind::unreachable_code, file, span_of(file, arm.form),
                   "an arm whose every case an arm before it takes");
        }
        remaining &= ~selected;

        auto const visible = scope.locals.size();
        if (is_narrowed && selected != 0)
        {
            narrowing const one[] = {{.name = text_of(file, name->where), .cases = selected}};
            narrow(scope, one);
        }
        auto arm_type = type_id::none;
        auto arm_exits = false;
        check_arm_body(scope, arm.result, yields_value, expected, arm_type, arm_exits, is_failed);
        scope.locals.resize_down_to(visible);
        every_arm_exits = every_arm_exits && arm_exits;
        if (!yields_value || arm_exits)
            continue;
        if (!is_valid(arm_type))
        {
            report(diagnostic_kind::missing_value_in_arm, file, span_of(file, arm.form),
                   "this case is a value, so every arm gives one or leaves");
            is_failed = true;
            continue;
        }
        if (arm_type == error_type)
        {
            is_failed = true;
            continue;
        }
        if (!is_valid(result) || result == error_type)
            result = arm_type;
        else if (arm_type != result)
            report(diagnostic_kind::type_mismatch, file, span_of(file, arm.form),
                   cc::format("this case is {}, and this arm gives a {}", out.name_of(result), out.name_of(arm_type)));
    }

    // CHK-551: every case an arm, or a `_`; and a value whose case nothing guarantees needs the `_` still
    auto is_exhaustive = has_wildcard;
    if (!has_wildcard && !is_failed)
    {
        if (remaining != 0)
            report(diagnostic_kind::non_exhaustive_case, file, where, case_names_of(scrutinee, remaining));
        else if (!t.is_covered)
            report(diagnostic_kind::non_exhaustive_case, file, where,
                   cc::format("the @whens of {} leave values that are in none of its cases, so this case needs a "
                              "`_` arm",
                              out.name_of(scrutinee)));
        else
            is_exhaustive = true;
    }
    if (ending != nullptr && is_exhaustive && every_arm_exits && arm_count > 0)
        *ending = flow::exits;
    if (is_failed)
        return error_type;
    if (!yields_value)
        return checked_module::void_type;
    return is_valid(result) ? result : error_type;
}

// ---- fields and casts -----------------------------------------------------------------------------------------------

cc::optional<type_id> checker::check_variant_member(i32 file, ast::expr_id id, ast::member const& member, type_id object)
{
    auto const& t = out.at(object);
    auto const name = text_of(file, member.name);
    auto const variants = out.at(t.variant_fields);
    auto const known = cases_of(file, member.object, object);
    auto any = isize(-1);
    for (auto i = isize(0); i < variants.size(); ++i)
    {
        if (variants[i].name != name)
            continue;
        any = i;
        if ((variants[i].leaves & known) == known)
        {
            set_target(file, id, {.kind = target_kind::variant_field, .symbol = t.symbol, .index = i32(i)});
            return variants[i].type;
        }
    }
    if (any < 0)
        return {};
    // CHK-543: a field of some cases is read where the value is known to be in them
    report(diagnostic_kind::unknown_member, file, member.name,
           cc::format("{} is a field of {} alone, and this value may be {}; narrow it with `is` first", name,
                      case_names_of(object, variants[any].leaves), case_names_of(object, known & ~variants[any].leaves)));
    return error_type;
}

type_id checker::check_case_cast(i32 file, ast::expr_id id, ast::cast const& node, type_id from, type_id to, u32 cases)
{
    if (from != to)
    {
        report(diagnostic_kind::no_matching_overload, file, span_of(file, id),
               cc::format("{} as a case of {}: no conversion", out.name_of(from), out.name_of(to)));
        return error_type;
    }
    // CHK-550: a value already known to be in the cases is one without a check
    auto const known = cases_of(file, node.value, from);
    set_cases(file, id, known & cases);
    // CHK-547: a value that holds no case of its own has nothing to check it by
    if ((known & ~cases) != 0 && !out.at(to).is_external_tag)
        set_target(file, id, {.kind = target_kind::struct_cases, .symbol = out.at(to).symbol, .index = i32(cases)});
    return to;
}

// ---- fields with a range ----------------------------------------------------------------------------------------------

member_info const* checker::ranged_member(type_id type, ast::field_id field) const
{
    if (!is_valid(type) || type == error_type || out.at(type).kind != type_kind::structure || !ast::is_valid(field))
        return nullptr;
    auto const& t = out.at(type);
    for (auto const& m : out.at(t.members))
        if (m.field == field && m.has_range)
            return &m;
    for (auto const& m : out.at(t.variant_fields))
        if (m.field == field && m.has_range)
            return &m;
    return nullptr;
}

void checker::judge_range(i32 file, ast::expr_id value, member_info const& field)
{
    // CHK-549: a constant outside the range is refused here; any other value is checked where it is stored (LEGAL-73)
    auto const number = number_of(file, value);
    if (!number.is_number || !number.is_integer || number.is_past_long)
        return;
    if (number.integer < field.range_first || number.integer > field.range_last)
        report(diagnostic_kind::literal_not_representable, file, span_of(file, value),
               cc::format("{} holds {} to {}, and not {}", field.name, field.range_first, field.range_last,
                          number.integer));
}
