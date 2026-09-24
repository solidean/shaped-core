#include "msl_reflection.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

namespace
{
using namespace cc::primitive_defines;

[[nodiscard]] bool is_identifier_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// Replaces every comment with spaces, so offsets stay put and nothing inside one is ever parsed.
[[nodiscard]] cc::string blank_comments(cc::string_view source)
{
    auto out = cc::string(source);
    for (auto i = isize(0); i < out.size(); ++i)
    {
        auto const two = out.size() - i >= 2;
        if (two && out[i] == '/' && out[i + 1] == '/')
        {
            while (i < out.size() && out[i] != '\n')
                out[i++] = ' ';
        }
        else if (two && out[i] == '/' && out[i + 1] == '*')
        {
            while (i < out.size() && !(i + 1 < out.size() && out[i] == '*' && out[i + 1] == '/'))
                out[i++] = ' ';
            if (i < out.size())
                out[i++] = ' ';
            if (i < out.size())
                out[i] = ' ';
        }
    }
    return out;
}

/// The whole-word occurrence of `word` in `text`, or -1.
/// Whole-word because `sampler` must not match inside `sampler_state`, and `device` not inside `device_index`.
[[nodiscard]] isize find_word(cc::string_view text, cc::string_view word, isize from = 0)
{
    for (auto at = text.find(word, from); at >= 0; at = text.find(word, at + 1))
    {
        auto const before_ok = at == 0 || !is_identifier_char(text[at - 1]);
        auto const after = at + word.size();
        auto const after_ok = after >= text.size() || !is_identifier_char(text[after]);
        if (before_ok && after_ok)
            return at;
    }
    return -1;
}

[[nodiscard]] bool has_word(cc::string_view text, cc::string_view word)
{
    return find_word(text, word) >= 0;
}

/// The index just past the matching close, given `open` sits on an opener.
[[nodiscard]] isize match_delimiter(cc::string_view text, isize open, char opener, char closer)
{
    auto depth = 0;
    for (auto i = open; i < text.size(); ++i)
    {
        if (text[i] == opener)
            ++depth;
        else if (text[i] == closer && --depth == 0)
            return i + 1;
    }
    return -1;
}

/// Splits on `separator`, ignoring any that sit inside brackets — a template argument list holds commas of its own.
[[nodiscard]] cc::vector<cc::string_view> split_top_level(cc::string_view text, char separator)
{
    auto parts = cc::vector<cc::string_view>();
    auto depth = 0;
    auto start = isize(0);
    for (auto i = isize(0); i < text.size(); ++i)
    {
        auto const c = text[i];
        if (c == '<' || c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == '>' || c == ')' || c == ']' || c == '}')
            --depth;
        else if (c == separator && depth == 0)
        {
            parts.push_back(text.subview({.start = start, .end = i}));
            start = i + 1;
        }
    }
    if (start < text.size())
        parts.push_back(text.subview({.start = start, .end = text.size()}));
    return parts;
}

[[nodiscard]] cc::string_view trimmed(cc::string_view text)
{
    auto start = isize(0);
    auto end = text.size();
    while (start < end && is_space(text[start]))
        ++start;
    while (end > start && is_space(text[end - 1]))
        --end;
    return text.subview({.start = start, .end = end});
}

/// The unsigned argument of `attribute(N)` inside an `[[...]]` list, or -1 when the attribute is absent.
[[nodiscard]] isize attribute_index(cc::string_view declaration, cc::string_view attribute)
{
    auto const at = find_word(declaration, attribute);
    if (at < 0)
        return -1;

    auto i = at + attribute.size();
    while (i < declaration.size() && is_space(declaration[i]))
        ++i;
    if (i >= declaration.size() || declaration[i] != '(')
        return -1;

    ++i;
    auto value = isize(0);
    auto digits = 0;
    while (i < declaration.size() && declaration[i] >= '0' && declaration[i] <= '9')
    {
        value = value * 10 + (declaration[i] - '0');
        ++i;
        ++digits;
    }
    return digits > 0 ? value : -1;
}

/// One declaration, split into the type and the declared name, with any `[[...]]` attributes removed.
struct declaration
{
    cc::string type;
    cc::string name;
    u32 count = 1;
    cc::string attributes;
};

/// Splits `text` — a struct member or a parameter, without its trailing punctuation — into type, name and array count.
[[nodiscard]] cc::optional<declaration> parse_declaration(cc::string_view text)
{
    auto result = declaration();

    // The attributes come off first, so the declarator is the last identifier in what remains.
    auto body = cc::string();
    for (auto i = isize(0); i < text.size(); ++i)
    {
        if (i + 1 < text.size() && text[i] == '[' && text[i + 1] == '[')
        {
            auto const close = match_delimiter(text, i, '[', ']');
            if (close < 0)
                return {};
            result.attributes += text.subview({.start = i, .end = close});
            result.attributes += " ";
            i = close - 1;
            continue;
        }
        body += text[i];
    }

    auto rest = trimmed(body);
    if (rest.empty())
        return {};

    // An array declarator binds to the name: `texture2d<float> tex[4]`.
    if (rest[rest.size() - 1] == ']')
    {
        auto open = rest.size() - 1;
        while (open > 0 && rest[open] != '[')
            --open;
        if (open == 0)
            return {};

        auto const inner = trimmed(rest.subview({.start = open + 1, .end = rest.size() - 1}));
        auto value = isize(0);
        for (auto i = isize(0); i < inner.size(); ++i)
        {
            if (inner[i] < '0' || inner[i] > '9')
                return {};
            value = value * 10 + (inner[i] - '0');
        }
        result.count = u32(value);
        rest = trimmed(rest.subview({.start = 0, .end = open}));
    }

    auto name_end = rest.size();
    while (name_end > 0 && !is_identifier_char(rest[name_end - 1]))
        --name_end;
    auto name_start = name_end;
    while (name_start > 0 && is_identifier_char(rest[name_start - 1]))
        --name_start;
    if (name_start == name_end)
        return {};

    result.name = cc::string(rest.subview({.start = name_start, .end = name_end}));
    result.type = cc::string(trimmed(rest.subview({.start = 0, .end = name_start})));
    return result.type.empty() ? cc::optional<declaration>() : cc::optional<declaration>(cc::move(result));
}

/// An argument-buffer struct: every member carries an `[[id(n)]]`.
struct argument_buffer
{
    cc::string name;
    cc::vector<declaration> members;
    cc::vector<u32> ids;
};

[[nodiscard]] cc::vector<argument_buffer> collect_argument_buffers(cc::string_view text)
{
    auto buffers = cc::vector<argument_buffer>();

    for (auto at = find_word(text, "struct"); at >= 0; at = find_word(text, "struct", at + 1))
    {
        auto i = at + isize(6);
        while (i < text.size() && is_space(text[i]))
            ++i;

        auto const name_start = i;
        while (i < text.size() && is_identifier_char(text[i]))
            ++i;
        if (name_start == i)
            continue;

        auto const name = text.subview({.start = name_start, .end = i});
        while (i < text.size() && is_space(text[i]))
            ++i;
        if (i >= text.size() || text[i] != '{')
            continue; // a forward declaration or a variable of a named struct type

        auto const body_end = match_delimiter(text, i, '{', '}');
        if (body_end < 0)
            continue;

        auto buffer = argument_buffer();
        buffer.name = cc::string(name);

        auto const body = text.subview({.start = i + 1, .end = body_end - 1});
        for (auto const member_text : split_top_level(body, ';'))
        {
            auto const member = parse_declaration(trimmed(member_text));
            if (!member.has_value())
                continue;

            auto const id = attribute_index(member.value().attributes, "id");
            if (id < 0)
                continue; // a plain struct member: this is only an argument buffer if its members are addressed

            buffer.members.push_back(member.value());
            buffer.ids.push_back(u32(id));
        }

        if (!buffer.members.empty())
            buffers.push_back(cc::move(buffer));
    }

    return buffers;
}
} // namespace

namespace
{
/// The buffer index sg's metal backend binds inline constants at, one past the reserved group.
/// It must equal `sg::backend::metal::k_inline_constants_buffer_index`, which ssc::msl cannot include.
constexpr auto k_inline_constants_buffer_index = isize(sg::reserved_binding_group + 1);

/// The texture kind a `texture*` type names, with `tex_2d` standing for the plain one.
[[nodiscard]] cc::optional<sg::texture_view_dimension> texture_dimension_of(cc::string_view type)
{
    // Longest first: `texture2d_array` also contains `texture2d`.
    if (has_word(type, "texturecube_array"))
        return sg::texture_view_dimension::cube_array;
    if (has_word(type, "texturecube"))
        return sg::texture_view_dimension::cube;
    if (has_word(type, "texture2d_ms_array"))
        return sg::texture_view_dimension::tex_2d_ms_array;
    if (has_word(type, "texture2d_ms"))
        return sg::texture_view_dimension::tex_2d_ms;
    if (has_word(type, "texture2d_array"))
        return sg::texture_view_dimension::tex_2d_array;
    if (has_word(type, "texture1d_array"))
        return sg::texture_view_dimension::tex_1d_array;
    if (has_word(type, "texture1d"))
        return sg::texture_view_dimension::tex_1d;
    if (has_word(type, "texture3d"))
        return sg::texture_view_dimension::tex_3d;
    if (has_word(type, "texture2d"))
        return sg::texture_view_dimension::tex_2d;
    return {};
}

/// What one declaration binds, as sg names it.
[[nodiscard]] cc::result<sg::binding> binding_of(declaration const& decl, cc::string_view where)
{
    auto const type = cc::string_view(decl.type);

    auto binding = sg::binding();
    binding.name = decl.name;
    binding.count = decl.count;

    if (auto const dimension = texture_dimension_of(type); dimension.has_value())
    {
        binding.texture_dimension = dimension;

        // `access::read` is the default when the type does not say, and only a writable one is a storage texture.
        auto const writes = has_word(type, "write") || has_word(type, "read_write");
        binding.type = writes ? sg::binding_type::readwrite_texture : sg::binding_type::readonly_texture;
        if (writes)
            binding.storage_access
                = has_word(type, "read_write") ? sg::storage_access::read_write : sg::storage_access::write;
        return binding;
    }

    if (has_word(type, "sampler"))
    {
        binding.type = sg::binding_type::sampler;
        return binding;
    }

    if (has_word(type, "acceleration_structure") || has_word(type, "instance_acceleration_structure")
        || has_word(type, "primitive_acceleration_structure"))
    {
        binding.type = sg::binding_type::acceleration_structure;
        return binding;
    }

    // A buffer, and which kind follows from the address space rather than from the pointee.
    if (has_word(type, "constant"))
    {
        binding.type = sg::binding_type::uniform_buffer;
        return binding;
    }

    if (has_word(type, "device") || has_word(type, "threadgroup"))
    {
        // Every `device T*` is structured: MSL spells a raw byte-addressed buffer exactly the same way, so the text
        // cannot tell them apart — see msl_reflection.hh.
        auto const readonly = has_word(type, "const");
        binding.type
            = readonly ? sg::binding_type::readonly_structured_buffer : sg::binding_type::readwrite_structured_buffer;
        return binding;
    }

    return cc::error(cc::format("'{}' in {} is of a kind sg has no binding_type for: '{}'", decl.name, where, type));
}

[[nodiscard]] isize start_of_line(cc::string_view text, isize at)
{
    while (at > 0 && text[at - 1] != '\n')
        --at;
    return at;
}

/// The three numbers of `numthreads x y z`, with `text` starting just past the word; absent unless all three are there.
[[nodiscard]] cc::optional<sg::compute_dimensions> parse_numthreads(cc::string_view text)
{
    auto values = cc::vector<i32>();
    auto i = isize(0);
    while (i < text.size() && values.size() < 3)
    {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
            ++i;
        auto value = 0;
        auto digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9')
        {
            value = value * 10 + (text[i] - '0');
            ++i;
            ++digits;
        }
        if (digits == 0)
            break;
        values.push_back(value);
    }
    if (values.size() != 3)
        return {};
    return sg::compute_dimensions{.x = values[0], .y = values[1], .z = values[2]};
}

/// The `#pragma sc numthreads x y z` among the lines directly above `signature_start`.
/// The search stops at the first line that is neither blank, a pragma nor an attribute, so a kernel never inherits the
/// shape of one declared before it.
[[nodiscard]] cc::optional<sg::compute_dimensions> numthreads_above(cc::string_view text, isize signature_start)
{
    auto next = signature_start;
    while (next > 0)
    {
        auto const start = start_of_line(text, next - 1);
        auto const line = trimmed(text.subview({.start = start, .end = next - 1}));
        next = start;

        if (line.empty() || line.starts_with("[["))
            continue;
        if (!line.starts_with("#"))
            break;

        // A preprocessor line: `#`, then `pragma`, `sc` and `numthreads` as words, any spacing between them.
        auto rest = trimmed(line.subview({.start = 1, .end = line.size()}));
        cc::string_view const words[] = {"pragma", "sc", "numthreads"};
        auto matched = true;
        for (auto const word : words)
        {
            if (!rest.starts_with(word) || (rest.size() > word.size() && is_identifier_char(rest[word.size()])))
            {
                matched = false;
                break;
            }
            rest = trimmed(rest.subview({.start = word.size(), .end = rest.size()}));
        }
        if (matched)
            return parse_numthreads(rest);
    }
    return {};
}
} // namespace

cc::result<cc::string_view> ssc::msl::impl::entry_qualifier_for(sg::shader_stage stage)
{
    switch (stage)
    {
    case sg::shader_stage::compute:
    case sg::shader_stage::raygen:
        // Metal schedules no raygen of its own: the kernel runs the traversal and calls what it hits.
        return cc::string_view("kernel");
    case sg::shader_stage::vertex:
        return cc::string_view("vertex");
    case sg::shader_stage::fragment:
        return cc::string_view("fragment");
    case sg::shader_stage::closest_hit:
    case sg::shader_stage::any_hit:
    case sg::shader_stage::miss:
    case sg::shader_stage::callable:
    case sg::shader_stage::intersection:
        // A shader table links these by name, and a visible function is what it can link.
        return cc::string_view("visible");
    case sg::shader_stage::tessellation_control:
    case sg::shader_stage::tessellation_evaluation:
        return cc::error("metal has no tessellation control or evaluation stage; it tessellates from a compute kernel "
                         "and a post-tessellation vertex function");
    case sg::shader_stage::geometry:
        return cc::error("metal has no geometry stage at all");
    }
    return cc::error("unknown shader stage");
}

cc::result<ssc::msl::impl::reflection> ssc::msl::impl::reflect(cc::string_view source,
                                                               cc::string_view entry_point,
                                                               sg::shader_stage stage)
{
    auto qualifier = entry_qualifier_for(stage);
    if (qualifier.has_error())
        return cc::error(cc::move(qualifier).error());

    auto const text_storage = blank_comments(source);
    auto const text = cc::string_view(text_storage);

    auto const buffers = collect_argument_buffers(text);

    // The entry point is the declaration of `entry_point` that is followed by a parameter list.
    auto entry_at = isize(-1);
    for (auto at = find_word(text, entry_point); at >= 0; at = find_word(text, entry_point, at + 1))
    {
        auto i = at + entry_point.size();
        while (i < text.size() && is_space(text[i]))
            ++i;
        if (i < text.size() && text[i] == '(')
        {
            entry_at = at;
            break;
        }
    }
    if (entry_at < 0)
        return cc::error(cc::format("the source declares no entry point named '{}'", entry_point));

    // What precedes the name carries the qualifier, and it must be the one this stage implies.
    auto const line_start = [&]
    {
        auto i = entry_at;
        auto seen = 0;
        // The signature's own line and the two above it, so a qualifier on a line of its own is still in view.
        while (i > 0 && seen < 3)
        {
            --i;
            if (text[i] == '\n')
                ++seen;
        }
        return i;
    }();

    auto const signature_head = text.subview({.start = line_start, .end = entry_at});
    auto qualifier_at = isize(-1);
    for (auto at = find_word(signature_head, qualifier.value()); at >= 0;
         at = find_word(signature_head, qualifier.value(), at + 1))
        qualifier_at = line_start + at;
    if (qualifier_at < 0)
        return cc::error(cc::format("'{}' is not declared as a `{}` function, which stage {} requires", entry_point,
                                    qualifier.value(), int(stage)));

    auto result = reflection();
    result.workgroup_size = numthreads_above(text, start_of_line(text, qualifier_at));

    // The parameters: each either binds a resource directly, or names an argument buffer whose members do.
    auto const params_open = entry_at + entry_point.size();
    auto const params_at = text.find('(', params_open);
    auto const params_end = match_delimiter(text, params_at, '(', ')');
    if (params_end < 0)
        return cc::error(cc::format("'{}' has no closing parenthesis on its parameter list", entry_point));

    auto const params = text.subview({.start = params_at + 1, .end = params_end - 1});
    for (auto const param_text : split_top_level(params, ','))
    {
        auto const param = parse_declaration(trimmed(param_text));
        if (!param.has_value())
            continue;

        auto const attributes = cc::string_view(param.value().attributes);
        auto const buffer_index = attribute_index(attributes, "buffer");
        auto const texture_index = attribute_index(attributes, "texture");
        auto const sampler_index = attribute_index(attributes, "sampler");

        // Everything else a parameter can carry — [[stage_in]], [[thread_position_in_grid]] and its neighbours —
        // is a value the hardware supplies rather than something a group binds.
        if (buffer_index < 0 && texture_index < 0 && sampler_index < 0)
            continue;

        if (buffer_index >= 0)
        {
            // An argument buffer: the parameter names a struct whose members carry the addresses.
            auto const* group = static_cast<argument_buffer const*>(nullptr);
            for (auto const& candidate : buffers)
            {
                if (has_word(param.value().type, candidate.name))
                    group = &candidate;
            }

            if (group != nullptr)
            {
                if (buffer_index > sg::reserved_binding_group)
                    return cc::error(
                        cc::format("'{}' of '{}' is an argument buffer at [[buffer({})]], and no group has "
                                   "that index: the groups are 0 to {}",
                                   param.value().name, entry_point, buffer_index, sg::reserved_binding_group));

                for (auto i = isize(0); i < group->members.size(); ++i)
                {
                    auto binding = binding_of(group->members[i], cc::string_view(group->name));
                    if (binding.has_error())
                        return cc::error(cc::move(binding).error());

                    binding.value().group_index = u32(buffer_index);
                    binding.value().index = group->ids[i];
                    binding.value().visibility = stage;
                    result.bindings.push_back(cc::move(binding.value()));
                }
                continue;
            }
        }

        // The one address the backend binds outside an argument buffer: the inline-constants block.
        // It reflects with no group and no space, which is how sg recognizes an inline block whatever its name.
        if (buffer_index == k_inline_constants_buffer_index)
        {
            auto binding = binding_of(param.value(), cc::string_view(entry_point));
            if (binding.has_error())
                return cc::error(cc::move(binding).error());
            if (binding.value().type != sg::binding_type::uniform_buffer)
                return cc::error(cc::format("'{}' of '{}' sits at the inline-constants index [[buffer({})]], so it "
                                            "must "
                                            "be a `constant T&`",
                                            param.value().name, entry_point, buffer_index));

            binding.value().visibility = stage;
            result.bindings.push_back(cc::move(binding.value()));
            continue;
        }

        // Anything else bound straight on the entry point is an address the backend never sets.
        auto kind = cc::string_view("sampler");
        auto index = sampler_index;
        if (texture_index >= 0)
        {
            kind = "texture";
            index = texture_index;
        }
        if (buffer_index >= 0)
        {
            kind = "buffer";
            index = buffer_index;
        }
        return cc::error(cc::format("'{}' of '{}' is bound directly at [[{}({})]], and metal binds resources through "
                                    "argument buffers only: make it a member of a struct bound at [[buffer(N)]], N "
                                    "being its group",
                                    param.value().name, entry_point, kind, index));
    }

    return result;
}
