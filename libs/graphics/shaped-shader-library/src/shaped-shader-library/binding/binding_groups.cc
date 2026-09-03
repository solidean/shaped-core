#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/binding/binding_groups.hh>
#include <shaped-shader-library/binding/impl/hlsl_binding_types.hh>
#include <shaped-shader-library/binding/impl/hlsl_tokens.hh>

using namespace cc::primitive_defines;

namespace
{
using slib::impl::annotation;
using slib::impl::hlsl_location;
using slib::impl::hlsl_token;
using slib::impl::hlsl_token_kind;
using slib::impl::to_string;

/// The attribute names the grammar knows.
/// A name outside this set is an error rather than a directive nobody reads — which is exactly what DXC makes of
/// it, since it ignores a pragma it does not know.
constexpr cc::string_view k_attribute_names[] = {"group", "static", "push_constants", "payload", "vertex_input"};

/// The attributes whose meaning has not landed yet.
/// Recognised so the failure names what is missing, rather than reading as an unknown word.
constexpr cc::string_view k_unimplemented_attributes[] = {"static", "push_constants", "payload", "vertex_input"};

/// HLSL constructs the pass cannot number, so they may not appear inside a group.
/// A shader that needs one moves it outside the namespace: the restriction is on where bindings are declared,
/// not on what a shader may contain.
constexpr cc::string_view k_rejected_keywords[]
    = {"namespace", "struct", "cbuffer", "tbuffer", "class", "typedef", "interface"};

/// What a source must carry before any of this can apply.
/// Deliberately only `#pragma` rather than `#pragma sc`: the flatten reprints a directive's tokens and nothing
/// promises it reprints the spacing between them, so which pragma it is, is the lexer's to decide.
/// A source with no pragma at all is passed through untouched, which is what makes "everything unannotated is byte
/// for byte" a property of the code rather than a claim about it.
constexpr cc::string_view k_pragma_marker = "#pragma";

[[nodiscard]] bool contains(cc::span<cc::string_view const> haystack, cc::string_view needle)
{
    for (auto const& candidate : haystack)
        if (candidate == needle)
            return true;
    return false;
}

/// A run of source bytes.
struct source_span
{
    isize offset = 0;
    isize length = 0;
};

/// One binding, plus where in the source its address has to be written.
/// The two arms write in different places — HLSL puts the address after the declared name and Vulkan before the
/// declaration — so both offsets are kept rather than one.
struct parsed_binding
{
    sg::binding binding;
    char register_class = 't';
    isize type_offset = 0;      ///< where the declaration's type token begins
    isize semicolon_offset = 0; ///< where its ';' is
};

struct parsed_group
{
    cc::string_view name;
    u32 group = 0;
    cc::vector<parsed_binding> bindings;
};

struct parsed_source
{
    cc::vector<parsed_group> groups;

    /// Every `#pragma sc` directive the parse consumed.
    /// The rewrite deletes them: DXC ignores an unknown pragma today, but `-Wall` promotes it to
    /// `-Wunknown-pragmas` and `-WX` makes that an error, so the compiler is never given the chance.
    cc::vector<source_span> annotations;
};

/// The parse of one translation unit.
/// Cursor state rather than a pure function, because an attribute stands on the line before the declaration it
/// applies to, so the parser has to carry one across.
struct parser
{
    cc::span<hlsl_token const> tokens;
    isize at = 0;

    cc::vector<source_span> annotations;

    // What has been declared so far, for the collisions a namespace does not catch on its own.
    cc::set<cc::string_view> group_names;
    cc::set<u32> group_numbers;
    cc::set<cc::string_view> binding_names;

    [[nodiscard]] bool at_end() const { return at >= tokens.size(); }
    [[nodiscard]] hlsl_token const& current() const { return tokens[at]; }

    [[nodiscard]] bool is_punctuation(char c) const
    {
        return !at_end() && current().kind == hlsl_token_kind::punctuation && current().text[0] == c;
    }

    [[nodiscard]] bool is_identifier(cc::string_view text) const
    {
        return !at_end() && current().kind == hlsl_token_kind::identifier && current().text == text;
    }

    /// The location to blame when the source ran out, which is the last one there was.
    [[nodiscard]] hlsl_location location_here() const
    {
        if (!at_end())
            return current().location;
        return tokens.empty() ? hlsl_location() : tokens.back().location;
    }

    /// Consumes the annotation at the cursor, rejecting a name the pass cannot honour.
    [[nodiscard]] cc::result<annotation> read_annotation()
    {
        auto const& token = current();
        auto parsed = slib::impl::parse_annotation(token.text, token.location);
        CC_RETURN_IF_ERROR(parsed);
        annotations.push_back({.offset = token.offset, .length = token.length});
        ++at;

        auto const& name = parsed.value().name;
        if (!contains(k_attribute_names, name))
            return cc::error(cc::format("{}: '{}' is not an attribute this pass knows", to_string(token.location), name));
        if (contains(k_unimplemented_attributes, name))
            return cc::error(cc::format("{}: the '{}' attribute is not supported yet", to_string(token.location), name));

        return parsed;
    }

    [[nodiscard]] cc::result<cc::vector<parsed_group>> run()
    {
        cc::vector<parsed_group> groups;

        // An attribute stands on its own line, so it waits here for the declaration it applies to.
        cc::optional<annotation> pending;

        while (!at_end())
        {
            if (current().kind == hlsl_token_kind::annotation)
            {
                auto const location = current().location;

                auto parsed = read_annotation();
                CC_RETURN_IF_ERROR(parsed);

                if (pending.has_value())
                    return cc::error(cc::format("{}: two attributes stand before one declaration", to_string(location)));

                pending = cc::move(parsed.value());
                continue;
            }

            if (is_identifier("namespace"))
            {
                auto group = parse_namespace(pending);
                CC_RETURN_IF_ERROR(group);
                pending = cc::nullopt;

                if (group.value().has_value())
                    groups.push_back(cc::move(group.value().value()));
                continue;
            }

            CC_RETURN_IF_ERROR(reject_unclaimed(pending));
            ++at;
        }

        CC_RETURN_IF_ERROR(reject_unclaimed(pending));
        return groups;
    }

    [[nodiscard]] static cc::result<cc::unit> reject_unclaimed(cc::optional<annotation> const& pending)
    {
        if (pending.has_value())
            return cc::error(cc::format("{}: a '{}' attribute must stand before a namespace declaration",
                                        to_string(pending.value().location), pending.value().name));
        return cc::unit();
    }

    /// Consumes one `namespace` declaration, returning the group when a `group` attribute stands before it.
    /// An unannotated namespace is walked into rather than skipped, so an attribute inside one is still found.
    [[nodiscard]] cc::result<cc::optional<parsed_group>> parse_namespace(cc::optional<annotation> const& pending)
    {
        auto const keyword_location = current().location;
        ++at; // `namespace`

        if (at_end() || current().kind != hlsl_token_kind::identifier)
        {
            CC_RETURN_IF_ERROR(reject_unclaimed(pending));
            return cc::optional<parsed_group>();
        }

        auto const name = current().text;
        auto const name_location = current().location;
        ++at;

        if (!pending.has_value())
            return cc::optional<parsed_group>();

        auto const& attribute = pending.value();
        if (attribute.name != "group")
            return cc::error(cc::format("{}: '{}' is not an attribute of a namespace", to_string(attribute.location),
                                        attribute.name));

        auto number = group_number_of(attribute);
        CC_RETURN_IF_ERROR(number);

        // One annotated namespace is declared exactly once, in one block, and owns its number alone — that is the
        // invariant that lets the build-time generator and the runtime rewriter agree without ever talking.
        if (!group_names.insert(name))
            return cc::error(cc::format("{}: namespace '{}' is declared twice", to_string(name_location), name));
        if (!group_numbers.insert(number.value()))
            return cc::error(cc::format("{}: group {} is declared twice, by namespace '{}'", to_string(name_location),
                                        number.value(), name));

        if (!is_punctuation('{'))
            return cc::error(
                cc::format("{}: namespace '{}' must open its block right away", to_string(keyword_location), name));
        ++at;

        auto bindings = parse_bindings(name);
        CC_RETURN_IF_ERROR(bindings);

        return cc::optional<parsed_group>(
            parsed_group{.name = name, .group = number.value(), .bindings = cc::move(bindings.value())});
    }

    /// The one argument `group` takes: the number, positional.
    [[nodiscard]] static cc::result<u32> group_number_of(annotation const& attribute)
    {
        if (attribute.arguments.size() != 1 || !attribute.arguments[0].key.empty()
            || attribute.arguments[0].values.size() != 1)
            return cc::error(cc::format("{}: 'group' takes exactly one number", to_string(attribute.location)));

        auto const number = cc::from_string<u32>(attribute.arguments[0].values[0]);
        if (!number.has_value())
            return cc::error(cc::format("{}: '{}' is not a group number", to_string(attribute.location),
                                        attribute.arguments[0].values[0]));
        return number.value();
    }

    /// The body of an annotated namespace, up to and including its closing brace.
    [[nodiscard]] cc::result<cc::vector<parsed_binding>> parse_bindings(cc::string_view group_name)
    {
        cc::vector<parsed_binding> bindings;
        u32 next_index = 0;

        while (true)
        {
            if (at_end())
                return cc::error(cc::format("{}: namespace '{}' is never closed", to_string(location_here()), group_name));

            if (is_punctuation('}'))
            {
                ++at;
                return bindings;
            }

            auto const& token = current();

            if (token.kind == hlsl_token_kind::annotation)
            {
                auto parsed = read_annotation();
                CC_RETURN_IF_ERROR(parsed);
                return cc::error(cc::format("{}: '{}' is not an attribute of a binding", to_string(token.location),
                                            parsed.value().name));
            }

            if (token.kind == hlsl_token_kind::punctuation && token.text[0] == '#')
                return cc::error(cc::format("{}: a preprocessor directive is not supported inside an annotated "
                                            "namespace",
                                            to_string(token.location)));

            if (token.kind != hlsl_token_kind::identifier)
                return cc::error(cc::format("{}: expected a binding declaration, found '{}'", to_string(token.location),
                                            token.text));

            if (contains(k_rejected_keywords, token.text))
                return cc::error(cc::format("{}: '{}' is not supported inside an annotated namespace",
                                            to_string(token.location), token.text));

            auto binding = parse_binding(next_index);
            CC_RETURN_IF_ERROR(binding);

            // An array consumes one index per element, because DXIL numbers every element while SPIR-V numbers the
            // array once — advancing by one would put the next binding at an address the two targets disagree on.
            next_index += binding.value().binding.count;
            bindings.push_back(cc::move(binding.value()));
        }
    }

    /// One `Type name;` or `Type name[N];`, at `index`.
    /// The declarator's shape is checked before the type is looked up, so a construct the subset excludes is named
    /// as what it is rather than as an unknown resource type.
    [[nodiscard]] cc::result<parsed_binding> parse_binding(u32 index)
    {
        auto const type_name = current().text;
        auto const type_offset = current().offset;
        auto const location = current().location;
        ++at;

        // The template arguments say what the resource holds, never where it is bound, so they are checked for
        // shape and then dropped.
        if (is_punctuation('<'))
        {
            ++at;
            while (!is_punctuation('>'))
            {
                if (at_end() || is_punctuation(';') || is_punctuation('{'))
                    return cc::error(
                        cc::format("{}: '{}' opens an argument list it never closes", to_string(location), type_name));
                if (is_punctuation('<'))
                    return cc::error(cc::format("{}: a nested template argument list is not supported",
                                                to_string(current().location)));
                ++at;
            }
            ++at; // the '>'
        }

        if (at_end() || current().kind != hlsl_token_kind::identifier)
            return cc::error(cc::format("{}: expected a name after '{}'", to_string(location), type_name));

        auto const name = current().text;
        ++at;

        u32 count = 1;
        if (is_punctuation('['))
        {
            ++at;
            if (at_end() || current().kind != hlsl_token_kind::number)
                return cc::error(
                    cc::format("{}: the length of '{}' must be a decimal literal", to_string(location), name));

            auto const length = cc::from_string<u32>(current().text);
            if (!length.has_value() || length.value() == 0)
                return cc::error(cc::format("{}: '{}' is not an array length", to_string(location), current().text));
            count = length.value();
            ++at;

            if (!is_punctuation(']'))
                return cc::error(cc::format("{}: '{}' never closes its array length", to_string(location), name));
            ++at;
        }

        if (is_punctuation('('))
            return cc::error(cc::format("{}: a function definition is not supported inside an annotated namespace",
                                        to_string(location)));
        if (is_punctuation(':'))
            return cc::error(cc::format("{}: '{}' must not write its own register — the pass owns the address",
                                        to_string(location), name));
        if (!is_punctuation(';'))
            return cc::error(cc::format("{}: expected ';' after '{}'", to_string(location), name));

        auto const semicolon_offset = current().offset;
        ++at;

        auto const type = slib::impl::binding_type_of(type_name);
        if (!type.has_value())
            return cc::error(
                cc::format("{}: '{}' is not a resource type this pass knows", to_string(location), type_name));

        // Reflection reports the bare name, so one name declared in two groups would reach sg as one binding at
        // two addresses — which a namespace does nothing to prevent.
        if (!binding_names.insert(name))
            return cc::error(cc::format("{}: '{}' is declared twice", to_string(location), name));

        return parsed_binding{.binding = {.name = cc::string::create_copy_of(name),
                                          .index = index,
                                          .count = count,
                                          .type = type.value().type,
                                          .texture_dimension = type.value().dimension},
                              .register_class = type.value().register_class,
                              .type_offset = type_offset,
                              .semicolon_offset = semicolon_offset};
    }
};

/// Every annotated namespace `hlsl` declares, with the group's number stamped onto each of its bindings.
[[nodiscard]] cc::result<parsed_source> parse_source(cc::string_view hlsl)
{
    if (!hlsl.contains(k_pragma_marker))
        return parsed_source();

    auto tokens = slib::impl::lex_hlsl(hlsl);
    CC_RETURN_IF_ERROR(tokens);

    parser p;
    p.tokens = tokens.value();

    auto groups = p.run();
    CC_RETURN_IF_ERROR(groups);

    // The group number is both the SPIR-V set and the HLSL space, which is what makes one address serve both targets.
    for (auto& group : groups.value())
        for (auto& binding : group.bindings)
        {
            binding.binding.group_index = group.group;
            binding.binding.space = group.group;
        }

    return parsed_source{.groups = cc::move(groups.value()), .annotations = cc::move(p.annotations)};
}

/// One replacement of `length` source bytes at `offset`.
/// The rewrite is a list of these rather than a rebuilt string, so every byte no edit names is provably the byte
/// that was there.
struct source_edit
{
    isize offset = 0;
    isize length = 0;
    cc::string text;
};

/// Applies edits that do not overlap, in offset order.
[[nodiscard]] cc::string apply_edits(cc::string_view source, cc::span<source_edit const> edits)
{
    cc::string result;
    result.reserve_back(source.size());

    isize copied = 0;
    for (auto const& edit : edits)
    {
        CC_ASSERT(edit.offset >= copied && edit.offset + edit.length <= source.size(),
                  "edits must be ordered by offset and lie inside the source");
        result += source.subview({.start = copied, .end = edit.offset});
        result += edit.text;
        copied = edit.offset + edit.length;
    }
    result += source.subview(copied);

    return result;
}
} // namespace

cc::result<cc::vector<slib::shader_binding_group>> slib::parse_binding_groups(cc::string_view hlsl)
{
    auto parsed = parse_source(hlsl);
    CC_RETURN_IF_ERROR(parsed);

    cc::vector<shader_binding_group> groups;
    groups.reserve(parsed.value().groups.size());

    for (auto& group : parsed.value().groups)
    {
        shader_binding_group result;
        result.name = cc::string::create_copy_of(group.name);
        result.group = group.group;
        result.bindings.reserve(group.bindings.size());
        for (auto& binding : group.bindings)
            result.bindings.push_back(cc::move(binding.binding));

        groups.push_back(cc::move(result));
    }

    return groups;
}

cc::result<cc::string> slib::rewrite_binding_groups(cc::string_view hlsl, sg::shader_format target)
{
    auto parsed = parse_source(hlsl);
    CC_RETURN_IF_ERROR(parsed);

    if (parsed.value().annotations.empty())
        return cc::string::create_copy_of(hlsl);

    // Only a source that carries an attribute needs an arm, so a target without one stays usable for every shader
    // that writes its own addresses.
    if (target != sg::shader_format::dxil && target != sg::shader_format::spirv)
        return cc::error("the binding preprocessor has no arm for this shader format");

    cc::vector<source_edit> edits;

    // The directives are removed, so nothing downstream meets a pragma it does not know.
    // Only their own bytes go and never the newline, which keeps every later line where the compiler's own
    // diagnostics will say it is.
    for (auto const& annotation : parsed.value().annotations)
        edits.push_back({.offset = annotation.offset, .length = annotation.length});

    for (auto const& group : parsed.value().groups)
        for (auto const& binding : group.bindings)
        {
            // HLSL puts the address after the declared name and Vulkan puts it before the declaration, which is
            // the asymmetry no prefix macro could bridge and the reason this is a rewriting pass at all.
            if (target == sg::shader_format::dxil)
                edits.push_back({.offset = binding.semicolon_offset,
                                 .text = cc::format(" : register({}{}, space{})", binding.register_class,
                                                    binding.binding.index, group.group)});
            else
                edits.push_back({.offset = binding.type_offset,
                                 .text = cc::format("[[vk::binding({}, {})]] ", binding.binding.index, group.group)});
        }

    cc::sort_by(edits, &source_edit::offset);
    return apply_edits(hlsl, edits);
}
