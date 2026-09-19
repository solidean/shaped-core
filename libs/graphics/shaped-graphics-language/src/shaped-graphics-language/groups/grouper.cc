#include "grouper.hh"

#include <clean-core/common/assert.hh>

namespace
{
using namespace sgl;

/// What the lines of one sibling chain are to the line above them.
enum class chain_mode : u8
{
    /// Each line starts a statement of a block.
    statements,
    /// Each line starts a new element of the paren that is open.
    elements,
    /// Each line goes on where the line above stopped.
    continuation,
};

/// A group that is still accepting children.
struct frame
{
    i32 group;
    i32 last_child = -1;
};

bool is_list(group_kind kind)
{
    return kind == group_kind::round || kind == group_kind::square || kind == group_kind::curly;
}

struct grouper
{
    parsed_file& file;
    cc::vector<frame> stack;
    /// Attributes seen and not yet given to a group; they go to the next group of the same unit.
    cc::vector<i32> pending_attributes;
    /// Above zero while inside an attribute's own argument list, where nothing is attached to anything.
    int attribute_depth = 0;
    /// The depth the lines being read may not close below: what is open there belongs to a line above them, and only
    /// that line's next sibling may close it.
    isize floor = 0;
    bool next_starts_line = false;
    bool next_is_fused = false;

    void report(diagnostic_kind kind, i32 token)
    {
        file.diagnostics.push_back({.kind = kind, .level = default_severity_of(kind), .where = file.tokens[token].where});
    }

    i32 make(group g)
    {
        file.groups.push_back(g);
        return i32(file.groups.size() - 1);
    }

    void attach_pending_to(i32 target)
    {
        auto* link = &file.groups[target].first_attribute;
        while (*link >= 0)
            link = &file.groups[*link].next_sibling;
        for (auto const attribute : pending_attributes)
        {
            *link = attribute;
            link = &file.groups[attribute].next_sibling;
        }
        pending_attributes.clear();
    }

    /// An attribute that nothing follows belongs to what came before it: the element a comma ends, the line a line end ends.
    void settle_trailing_attributes()
    {
        if (pending_attributes.empty() || attribute_depth > 0 || stack.back().last_child < 0)
            return;
        for (auto const attribute : pending_attributes)
            file.groups[attribute].is_trailing = true;
        attach_pending_to(stack.back().last_child);
    }

    void append(i32 index)
    {
        auto& top = stack.back();
        auto const previous = top.last_child;
        if (top.last_child >= 0)
            file.groups[top.last_child].next_sibling = index;
        else
            file.groups[top.group].first_child = index;
        top.last_child = index;

        // A statement is a container rather than something written, so an attribute waits for the first real group.
        auto const is_written = file.groups[index].kind != group_kind::statement;
        if (attribute_depth == 0 && is_written && !pending_attributes.empty())
        {
            // Outside a paren an attribute leads its line, or follows a marker whose operand it annotates.
            auto const in_line = file.groups[top.group].kind == group_kind::statement;
            if (in_line && previous >= 0 && !is_marker(previous))
                report(diagnostic_kind::misplaced_attribute, file.groups[pending_attributes[0]].token);
            attach_pending_to(index);
        }
    }

    /// `:`, `->`, `=>`, `as` and `in`: what follows one is an operand that may carry attributes of its own.
    [[nodiscard]] bool is_marker(i32 g) const
    {
        if (file.groups[g].kind != group_kind::token)
            return false;
        auto const& tok = file.tokens[file.groups[g].token];
        if (tok.kind == token_kind::colon || tok.kind == token_kind::arrow || tok.kind == token_kind::double_arrow)
            return true;
        auto const word = file.text_of(tok.where);
        return tok.kind == token_kind::symbol && (word == "as" || word == "in");
    }

    i32 append_new(group g)
    {
        g.starts_line = next_starts_line;
        next_starts_line = false;
        auto const index = make(g);
        append(index);
        return index;
    }

    void pop()
    {
        stack.remove_back();
        if (!stack.empty() && file.groups[stack.back().group].kind == group_kind::attribute)
        {
            stack.remove_back();
            --attribute_depth;
        }
    }

    /// Closes by force whatever is open above `depth`, reporting each paren that never met its closer.
    /// An open quote is not reported: the tokenizer already said what was wrong with it.
    void close_down_to(isize depth, bool is_reported_already = false)
    {
        while (stack.size() > depth)
        {
            auto const& g = file.groups[stack.back().group];
            if (is_list(g.kind) && g.close_token < 0 && !is_reported_already)
                report(diagnostic_kind::missing_closer, g.token);
            pop();
        }
    }

    [[nodiscard]] bool top_is_open_string() const
    {
        auto const& g = file.groups[stack.back().group];
        return g.kind == group_kind::quoted && g.close_token < 0;
    }

    void take_token(line const& l, i32 t)
    {
        auto const& tok = file.tokens[t];
        auto const is_first_of_line = t == i32(l.first_token);
        auto g = group{
            .is_fused_left = next_is_fused || (!is_first_of_line && file.is_fused_left(t)),
            .token = t,
        };
        next_is_fused = false;

        auto const matching_list = [&]() -> group_kind
        {
            switch (tok.kind)
            {
            case token_kind::round_open:
            case token_kind::round_close:
                return group_kind::round;
            case token_kind::square_open:
            case token_kind::square_close:
                return group_kind::square;
            default:
                return group_kind::curly;
            }
        };

        switch (tok.kind)
        {
        case token_kind::comment:
            return;

        case token_kind::round_open:
        case token_kind::square_open:
        case token_kind::curly_open:
            g.kind = matching_list();
            stack.push_back({.group = append_new(g)});
            return;

        case token_kind::round_close:
        case token_kind::square_close:
        case token_kind::curly_close:
        {
            auto& open = file.groups[stack.back().group];
            if (stack.size() <= floor || open.kind != matching_list() || open.close_token >= 0)
            {
                report(diagnostic_kind::unmatched_closer, t);
                return;
            }
            settle_trailing_attributes();
            open.close_token = t;
            pop();
            return;
        }

        case token_kind::quote_open:
            g.kind = group_kind::quoted;
            stack.push_back({.group = append_new(g)});
            return;

        case token_kind::quote_close:
            if (stack.size() <= floor || !top_is_open_string())
            {
                report(diagnostic_kind::unmatched_closer, t);
                return;
            }
            file.groups[stack.back().group].close_token = t;
            pop();
            return;

        case token_kind::comma:
            settle_trailing_attributes();
            break;

        case token_kind::symbol:
            if (file.text_of(tok.where).starts_with('@'))
            {
                g.kind = group_kind::attribute;
                auto const attribute = make(g);
                pending_attributes.push_back(attribute);

                auto const has_arguments = t + 1 < i32(l.first_token + l.token_count)
                                        && file.tokens[t + 1].kind == token_kind::round_open
                                        && file.is_fused_left(t + 1);
                if (has_arguments)
                {
                    stack.push_back({.group = attribute});
                    ++attribute_depth;
                }
                else if (t + 1 < i32(l.first_token + l.token_count) && file.tokens[t + 1].kind == token_kind::round_open)
                    report(diagnostic_kind::spaced_attribute_arguments, t);
                return;
            }
            break;

        default:
            break;
        }

        append_new(g);
    }

    /// Every line under a multi-line opener is content, however deep, in source order.
    void take_string_content(i32 first_line)
    {
        for (auto i = first_line; i >= 0; i = file.lines[i].next_sibling)
        {
            auto const& l = file.lines[i];
            auto const depth = stack.size();
            next_starts_line = true;
            for (auto t = i32(l.first_token); t < i32(l.first_token + l.token_count); ++t)
                take_token(l, t);
            // An interpolation ends with its line, and the tokenizer has already said so if it did not close.
            close_down_to(depth, true);
            next_starts_line = false;
            take_string_content(l.first_child);
        }
    }

    [[nodiscard]] bool has_content(i32 first_line) const
    {
        for (auto i = first_line; i >= 0; i = file.lines[i].next_sibling)
            if (file.lines[i].kind != line_kind::blank)
                return true;
        return false;
    }

    void take_line(i32 index, isize unit_depth, chain_mode mode)
    {
        auto const& l = file.lines[index];
        auto const first = i32(l.first_token);
        auto last = first + i32(l.token_count) - 1;
        while (last >= first && file.tokens[last].kind == token_kind::comment)
            --last;
        auto const has_block_colon = last >= first && file.tokens[last].kind == token_kind::colon;

        for (auto t = first; t <= (has_block_colon ? last - 1 : last); ++t)
            take_token(l, t);

        // A string still open on a line that opens none is undelimited, and ends with the line.
        // An interpolation it left open goes with it, and the tokenizer has reported both already.
        if (!l.opens_string)
            for (auto depth = stack.size() - 1; depth >= unit_depth; --depth)
            {
                auto const& g = file.groups[stack[depth].group];
                if (g.kind == group_kind::quoted && g.close_token < 0)
                {
                    close_down_to(depth, true);
                    break;
                }
            }

        if (has_block_colon)
        {
            settle_trailing_attributes();
            auto const block = append_new({.kind = group_kind::block, .token = last});
            if (!has_content(l.first_child))
                report(diagnostic_kind::empty_block, last);

            stack.push_back({.group = block});
            run_chain(l.first_child, chain_mode::statements);
            stack.remove_back();
        }
        else if (top_is_open_string())
            take_string_content(l.first_child);
        else if (is_list(file.groups[stack.back().group].kind))
            run_chain(l.first_child, chain_mode::elements);
        else
        {
            // A continuation that continues again reads as nesting that is not there.
            if (mode == chain_mode::continuation)
                for (auto child = l.first_child; child >= 0; child = file.lines[child].next_sibling)
                    if (file.lines[child].kind == line_kind::code && first_significant_token(file.lines[child]) >= 0)
                    {
                        report(diagnostic_kind::nested_continuation, first_significant_token(file.lines[child]));
                        break;
                    }
            run_chain(l.first_child, chain_mode::continuation);
        }

        if (stack.size() == unit_depth)
            settle_trailing_attributes();
    }

    [[nodiscard]] i32 first_significant_token(line const& l) const
    {
        for (auto t = i32(l.first_token); t < i32(l.first_token + l.token_count); ++t)
            if (file.tokens[t].kind != token_kind::comment)
                return t;
        return -1;
    }

    /// True if `t` closes the innermost thing that is open.
    [[nodiscard]] bool closes_innermost(i32 t) const
    {
        auto const& open = file.groups[stack.back().group];
        switch (file.tokens[t].kind)
        {
        case token_kind::quote_close:
            return open.kind == group_kind::quoted;
        case token_kind::round_close:
            return open.kind == group_kind::round;
        case token_kind::square_close:
            return open.kind == group_kind::square;
        case token_kind::curly_close:
            return open.kind == group_kind::curly;
        default:
            return false;
        }
    }

    /// True if the line opens with an operator that needs an operand on its left.
    /// No element can start that way, so such a line continues the element above it — which is what lets a condition
    /// wrapped in parens break before each `and`.
    /// A prefix operator is fused to what follows it and so never qualifies: `-x` on its own line is an element.
    [[nodiscard]] bool starts_with_infix(line const& l, i32 first_token) const
    {
        auto const& tok = file.tokens[first_token];
        switch (tok.kind)
        {
        case token_kind::colon:
        case token_kind::arrow:
        case token_kind::double_arrow:
            return true;
        case token_kind::symbol:
        {
            auto const word = file.text_of(tok.where);
            return word == "and" || word == "or" || word == "as" || word == "in";
        }
        case token_kind::op:
        {
            auto const next = first_token + 1;
            auto const has_next
                = next < i32(l.first_token + l.token_count) && file.tokens[next].kind != token_kind::comment;
            return !has_next || !file.is_fused_left(next);
        }
        default:
            return false;
        }
    }

    void run_chain(i32 first_line, chain_mode mode)
    {
        auto const base = stack.size();
        auto const unit_depth = mode == chain_mode::statements ? base + 1 : base;
        auto const outer_floor = floor;
        auto owes_closer = false;

        for (auto i = first_line; i >= 0; i = file.lines[i].next_sibling)
        {
            auto const& l = file.lines[i];
            if (l.kind != line_kind::code)
                continue;
            auto const first_token = first_significant_token(l);
            if (first_token < 0)
                continue; // a comment-only line, which owns its children as comments

            if (owes_closer && !closes_innermost(first_token))
            {
                close_down_to(unit_depth);
                owes_closer = false;
            }

            if (!owes_closer)
            {
                close_down_to(base);
                if (mode == chain_mode::statements)
                    stack.push_back({.group = append_new({.kind = group_kind::statement, .token = first_token})});
                else if (mode == chain_mode::elements)
                    next_starts_line = !starts_with_infix(l, first_token);
                else
                    next_is_fused = file.tokens[first_token].kind == token_kind::dot;
            }

            floor = unit_depth;
            take_line(i, unit_depth, mode);
            owes_closer = stack.size() > unit_depth;
        }

        close_down_to(base);
        // An attribute never leaves the block it was written in.
        if (mode == chain_mode::statements)
        {
            for (auto const attribute : pending_attributes)
                report(diagnostic_kind::unattached_attribute, file.groups[attribute].token);
            pending_attributes.clear();
        }
        floor = outer_floor;
        next_starts_line = false;
        next_is_fused = false;
    }
};
} // namespace

void sgl::group_tokens(parsed_file& file)
{
    CC_ASSERT(file.groups.empty(), "a file is grouped once");

    auto g = grouper{.file = file};
    file.root_block = g.make({.kind = group_kind::block});
    g.stack.push_back({.group = file.root_block});
    g.run_chain(file.first_line, chain_mode::statements);
}
