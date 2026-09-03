#include <clean-core/container/set.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <shaped-shader-library/binding/binding_groups.hh>
#include <shaped-shader-library/binding/impl/hlsl_binding_types.hh>
#include <shaped-shader-library/binding/impl/hlsl_tokens.hh>

using namespace cc::primitive_defines;

namespace
{
using slib::impl::annotation;
using slib::impl::hlsl_token;
using slib::impl::hlsl_token_kind;

/// The attribute names the grammar knows.
/// A name outside this set is an error rather than a comment, so a typo cannot silently disable a binding.
constexpr cc::string_view k_attribute_names[] = {"group", "static", "push_constants", "payload", "vertex_input"};

/// The attributes whose meaning has not landed yet.
/// Recognised so the failure names what is missing, rather than reading as an unknown word.
constexpr cc::string_view k_unimplemented_attributes[] = {"static", "push_constants", "payload", "vertex_input"};

/// HLSL constructs the pass cannot number, so they may not appear inside a group.
/// A shader that needs one moves it outside the namespace: the restriction is on where bindings are declared,
/// not on what a shader may contain.
constexpr cc::string_view k_rejected_keywords[]
    = {"namespace", "struct", "cbuffer", "tbuffer", "class", "typedef", "interface"};

[[nodiscard]] bool contains(cc::span<cc::string_view const> haystack, cc::string_view needle)
{
    for (auto const& candidate : haystack)
        if (candidate == needle)
            return true;
    return false;
}

/// The parse of one translation unit.
/// Cursor state rather than a pure function, because the attachment rule needs a token of lookahead in two
/// directions: a trailing attribute follows its declaration, a standalone one precedes it.
struct parser
{
    cc::span<hlsl_token const> tokens;
    isize at = 0;

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

    /// The line to blame when the source ran out, which is the last line there was.
    [[nodiscard]] i32 line_here() const
    {
        if (!at_end())
            return current().line;
        return tokens.empty() ? 1 : tokens.back().line;
    }

    /// Consumes the annotation at the cursor, rejecting a name the pass cannot honour.
    [[nodiscard]] cc::result<annotation> read_annotation()
    {
        auto const& token = current();
        auto parsed = slib::impl::parse_annotation(token.text, token.line);
        CC_RETURN_IF_ERROR(parsed);
        ++at;

        auto const& name = parsed.value().name;
        if (!contains(k_attribute_names, name))
            return cc::error(cc::format("line {}: '{}' is not an attribute this pass knows", token.line, name));
        if (contains(k_unimplemented_attributes, name))
            return cc::error(cc::format("line {}: the '{}' attribute is not supported yet", token.line, name));

        return parsed;
    }

    [[nodiscard]] cc::result<cc::vector<slib::shader_binding_group>> run()
    {
        cc::vector<slib::shader_binding_group> groups;

        // An attribute standing alone on its line waits here for the declaration it attaches to.
        cc::optional<annotation> pending;

        while (!at_end())
        {
            if (current().kind == hlsl_token_kind::annotation)
            {
                auto const line = current().line;
                auto const first_on_line = current().first_on_line;

                auto parsed = read_annotation();
                CC_RETURN_IF_ERROR(parsed);

                // Reaching a trailing attribute here means what it rode along with was not a namespace, since a
                // namespace claims its own before the loop comes back around.
                if (!first_on_line)
                    return cc::error(cc::format("line {}: a '{}' attribute must be on a namespace declaration", line,
                                                parsed.value().name));
                if (pending.has_value())
                    return cc::error(cc::format("line {}: two attributes stand before one declaration", line));

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
            return cc::error(cc::format("line {}: a '{}' attribute must be on a namespace declaration",
                                        pending.value().line, pending.value().name));
        return cc::unit();
    }

    /// Consumes one `namespace` declaration, returning the group when it carries a `group` attribute.
    /// An unannotated namespace is walked into rather than skipped, so an attribute inside one is still found.
    [[nodiscard]] cc::result<cc::optional<slib::shader_binding_group>> parse_namespace(cc::optional<annotation> const& pending)
    {
        auto const keyword_line = current().line;
        ++at; // `namespace`

        if (at_end() || current().kind != hlsl_token_kind::identifier)
        {
            CC_RETURN_IF_ERROR(reject_unclaimed(pending));
            return cc::optional<slib::shader_binding_group>();
        }

        auto const name = current().text;
        auto const name_line = current().line;
        ++at;

        auto attribute = pending;
        if (!at_end() && current().kind == hlsl_token_kind::annotation && !current().first_on_line)
        {
            auto trailing = read_annotation();
            CC_RETURN_IF_ERROR(trailing);
            if (attribute.has_value())
                return cc::error(cc::format("line {}: namespace '{}' carries two attributes", name_line, name));
            attribute = cc::move(trailing.value());
        }

        if (!attribute.has_value())
            return cc::optional<slib::shader_binding_group>();

        if (attribute.value().name != "group")
            return cc::error(cc::format("line {}: '{}' is not an attribute of a namespace", attribute.value().line,
                                        attribute.value().name));

        auto number = group_number_of(attribute.value());
        CC_RETURN_IF_ERROR(number);

        // One annotated namespace is declared exactly once, in one block, and owns its number alone — that is the
        // invariant that lets the build-time generator and the runtime rewriter agree without ever talking.
        if (!group_names.insert(name))
            return cc::error(cc::format("line {}: namespace '{}' is declared twice", name_line, name));
        if (!group_numbers.insert(number.value()))
            return cc::error(
                cc::format("line {}: group {} is declared twice, by namespace '{}'", name_line, number.value(), name));

        if (!is_punctuation('{'))
            return cc::error(cc::format("line {}: namespace '{}' must open its block right away", keyword_line, name));
        ++at;

        auto bindings = parse_bindings(name);
        CC_RETURN_IF_ERROR(bindings);

        return cc::optional<slib::shader_binding_group>(
            slib::shader_binding_group{.name = cc::string::create_copy_of(name),
                                       .group = number.value(),
                                       .bindings = cc::move(bindings.value())});
    }

    /// The one argument `group` takes: the number, positional.
    [[nodiscard]] static cc::result<u32> group_number_of(annotation const& attribute)
    {
        if (attribute.arguments.size() != 1 || !attribute.arguments[0].key.empty()
            || attribute.arguments[0].values.size() != 1)
            return cc::error(cc::format("line {}: 'group' takes exactly one number", attribute.line));

        auto const number = cc::from_string<u32>(attribute.arguments[0].values[0]);
        if (!number.has_value())
            return cc::error(
                cc::format("line {}: '{}' is not a group number", attribute.line, attribute.arguments[0].values[0]));
        return number.value();
    }

    /// The body of an annotated namespace, up to and including its closing brace.
    [[nodiscard]] cc::result<cc::vector<sg::binding>> parse_bindings(cc::string_view group_name)
    {
        cc::vector<sg::binding> bindings;
        u32 next_index = 0;

        while (true)
        {
            if (at_end())
                return cc::error(cc::format("line {}: namespace '{}' is never closed", line_here(), group_name));

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
                return cc::error(
                    cc::format("line {}: a '{}' attribute stands before no binding", token.line, parsed.value().name));
            }

            if (token.kind == hlsl_token_kind::punctuation && token.text[0] == '#')
                return cc::error(cc::format(
                    "line {}: a preprocessor directive is not supported inside an annotated namespace", token.line));

            if (token.kind != hlsl_token_kind::identifier)
                return cc::error(
                    cc::format("line {}: expected a binding declaration, found '{}'", token.line, token.text));

            if (contains(k_rejected_keywords, token.text))
                return cc::error(
                    cc::format("line {}: '{}' is not supported inside an annotated namespace", token.line, token.text));

            auto binding = parse_binding(next_index);
            CC_RETURN_IF_ERROR(binding);

            // An array consumes one index per element, because DXIL numbers every element while SPIR-V numbers the
            // array once — advancing by one would put the next binding at an address the two targets disagree on.
            next_index += binding.value().count;
            bindings.push_back(cc::move(binding.value()));
        }
    }

    /// One `Type name;` or `Type name[N];`, at `index`.
    /// The declarator's shape is checked before the type is looked up, so a construct the subset excludes is named
    /// as what it is rather than as an unknown resource type.
    [[nodiscard]] cc::result<sg::binding> parse_binding(u32 index)
    {
        auto const type_name = current().text;
        auto const line = current().line;
        ++at;

        // The template arguments say what the resource holds, never where it is bound, so they are checked for
        // shape and then dropped.
        if (is_punctuation('<'))
        {
            ++at;
            while (!is_punctuation('>'))
            {
                if (at_end() || is_punctuation(';') || is_punctuation('{'))
                    return cc::error(cc::format("line {}: '{}' opens an argument list it never closes", line, type_name));
                if (is_punctuation('<'))
                    return cc::error(
                        cc::format("line {}: a nested template argument list is not supported", current().line));
                ++at;
            }
            ++at; // the '>'
        }

        if (at_end() || current().kind != hlsl_token_kind::identifier)
            return cc::error(cc::format("line {}: expected a name after '{}'", line, type_name));

        auto const name = current().text;
        ++at;

        u32 count = 1;
        if (is_punctuation('['))
        {
            ++at;
            if (at_end() || current().kind != hlsl_token_kind::number)
                return cc::error(cc::format("line {}: the length of '{}' must be a decimal literal", line, name));

            auto const length = cc::from_string<u32>(current().text);
            if (!length.has_value() || length.value() == 0)
                return cc::error(cc::format("line {}: '{}' is not an array length", line, current().text));
            count = length.value();
            ++at;

            if (!is_punctuation(']'))
                return cc::error(cc::format("line {}: '{}' never closes its array length", line, name));
            ++at;
        }

        if (is_punctuation('('))
            return cc::error(
                cc::format("line {}: a function definition is not supported inside an annotated namespace", line));
        if (is_punctuation(':'))
            return cc::error(
                cc::format("line {}: '{}' must not write its own register — the pass owns the address", line, name));
        if (!is_punctuation(';'))
            return cc::error(cc::format("line {}: expected ';' after '{}'", line, name));
        ++at;

        // A trailing attribute rides along on the declaration's own line.
        if (!at_end() && current().kind == hlsl_token_kind::annotation && !current().first_on_line)
        {
            auto parsed = read_annotation();
            CC_RETURN_IF_ERROR(parsed);
            return cc::error(
                cc::format("line {}: '{}' is not an attribute of a binding", parsed.value().line, parsed.value().name));
        }

        auto const type = slib::impl::binding_type_of(type_name);
        if (!type.has_value())
            return cc::error(cc::format("line {}: '{}' is not a resource type this pass knows", line, type_name));

        // Reflection reports the bare name, so one name declared in two groups would reach sg as one binding at
        // two addresses — which a namespace does nothing to prevent.
        if (!binding_names.insert(name))
            return cc::error(cc::format("line {}: '{}' is declared twice", line, name));

        return sg::binding{.name = cc::string::create_copy_of(name),
                           .index = index,
                           .count = count,
                           .type = type.value().type,
                           .texture_dimension = type.value().dimension};
    }
};
} // namespace

cc::result<cc::vector<slib::shader_binding_group>> slib::parse_binding_groups(cc::string_view hlsl)
{
    auto tokens = impl::lex_hlsl(hlsl);
    CC_RETURN_IF_ERROR(tokens);

    parser p;
    p.tokens = tokens.value();

    auto groups = p.run();
    CC_RETURN_IF_ERROR(groups);

    // The group number is both the SPIR-V set and the HLSL space, which is what makes one address serve both targets.
    for (auto& group : groups.value())
        for (auto& binding : group.bindings)
        {
            binding.group_index = group.group;
            binding.space = group.group;
        }

    return groups;
}
