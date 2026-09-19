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
    group_id group;
    group_id last_child = group_id::none;
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
    cc::vector<group_id> pending_attributes;
    /// Above zero while inside an attribute's own argument list, where nothing is attached to anything.
    int attribute_depth = 0;
    /// The depth the lines being read may not close below: what is open there belongs to a line above them, and only
    /// that line's next sibling may close it.
    isize floor = 0;
    bool next_starts_line = false;
    bool next_is_fused = false;

    void report(diagnostic_kind kind, token_id token)
    {
        file.diagnostics.push_back({.kind = kind, .level = default_severity_of(kind), .where = file.at(token).where});
    }

    group_id make(group g)
    {
        file.groups.push_back(g);
        return group_id(i32(file.groups.size() - 1));
    }

    void attach_pending_to(group_id target)
    {
        auto* link = &file.at(target).first_attribute;
        while (is_valid(*link))
            link = &file.at(*link).next_sibling;
        for (auto const attribute : pending_attributes)
        {
            *link = attribute;
            link = &file.at(attribute).next_sibling;
        }
        pending_attributes.clear();
    }

    /// An attribute that nothing follows belongs to what came before it: the element a comma ends, the line a line end ends.
    void settle_trailing_attributes()
    {
        if (pending_attributes.empty() || attribute_depth > 0 || !is_valid(stack.back().last_child))
            return;
        for (auto const attribute : pending_attributes)
            file.at(attribute).is_trailing = true;
        attach_pending_to(stack.back().last_child);
    }

    void append(group_id id)
    {
        auto& top = stack.back();
        auto const previous = top.last_child;
        if (is_valid(top.last_child))
            file.at(top.last_child).next_sibling = id;
        else
            file.at(top.group).first_child = id;
        top.last_child = id;

        // A statement is a container rather than something written, so an attribute waits for the first real group.
        auto const is_written = file.at(id).kind != group_kind::statement;
        if (attribute_depth == 0 && is_written && !pending_attributes.empty())
        {
            // Outside a paren an attribute leads its line, or follows a marker whose operand it annotates.
            auto const in_line = file.at(top.group).kind == group_kind::statement;
            if (in_line && is_valid(previous) && !is_marker(previous))
                report(diagnostic_kind::misplaced_attribute, file.at(pending_attributes[0]).token);
            attach_pending_to(id);
        }
    }

    /// `:`, `->`, `=>`, `as` and `in`: what follows one is an operand that may carry attributes of its own.
    [[nodiscard]] bool is_marker(group_id g) const
    {
        if (file.at(g).kind != group_kind::token)
            return false;
        auto const& tok = file.at(file.at(g).token);
        if (tok.kind == token_kind::colon || tok.kind == token_kind::arrow || tok.kind == token_kind::double_arrow)
            return true;
        auto const word = file.text_of(tok.where);
        return tok.kind == token_kind::symbol && (word == "as" || word == "in");
    }

    group_id append_new(group g)
    {
        g.starts_line = next_starts_line;
        next_starts_line = false;
        auto const id = make(g);
        append(id);
        return id;
    }

    void pop()
    {
        stack.remove_back();
        if (!stack.empty() && file.at(stack.back().group).kind == group_kind::attribute)
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
            auto const& g = file.at(stack.back().group);
            if (is_list(g.kind) && !is_valid(g.close_token) && !is_reported_already)
                report(diagnostic_kind::missing_closer, g.token);
            pop();
        }
    }

    [[nodiscard]] bool top_is_open_string() const
    {
        auto const& g = file.at(stack.back().group);
        return g.kind == group_kind::quoted && !is_valid(g.close_token);
    }

    void take_token(line const& l, token_id t)
    {
        auto const& tok = file.at(t);
        auto const is_first_of_line = t == l.tokens_begin();
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
            auto& open = file.at(stack.back().group);
            if (stack.size() <= floor || open.kind != matching_list() || is_valid(open.close_token))
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
            file.at(stack.back().group).close_token = t;
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

                auto const after = next(t);
                auto const has_list = after < l.tokens_end() && file.at(after).kind == token_kind::round_open;
                auto const has_arguments = has_list && file.is_fused_left(after);
                if (has_arguments)
                {
                    stack.push_back({.group = attribute});
                    ++attribute_depth;
                }
                else if (has_list)
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
    void take_string_content(line_id first_line)
    {
        for (auto i = first_line; is_valid(i); i = file.at(i).next_sibling)
        {
            auto const& l = file.at(i);
            auto const depth = stack.size();
            next_starts_line = true;
            for (auto t = l.tokens_begin(); t < l.tokens_end(); t = next(t))
                take_token(l, t);
            // An interpolation ends with its line, and the tokenizer has already said so if it did not close.
            close_down_to(depth, true);
            next_starts_line = false;
            take_string_content(l.first_child);
        }
    }

    [[nodiscard]] bool has_content(line_id first_line) const
    {
        for (auto i = first_line; is_valid(i); i = file.at(i).next_sibling)
            if (file.at(i).kind != line_kind::blank)
                return true;
        return false;
    }

    void take_line(line_id id, isize unit_depth, chain_mode mode)
    {
        auto const& l = file.at(id);
        auto const first = l.tokens_begin();
        // One past the last token that is not a comment.
        auto end = l.tokens_end();
        while (end > first && file.at(previous(end)).kind == token_kind::comment)
            end = previous(end);
        auto const has_block_colon = end > first && file.at(previous(end)).kind == token_kind::colon;
        auto const block_colon = has_block_colon ? previous(end) : token_id::none;

        for (auto t = first; t < (has_block_colon ? block_colon : end); t = next(t))
            take_token(l, t);

        // A string still open on a line that opens none is undelimited, and ends with the line.
        // An interpolation it left open goes with it, and the tokenizer has reported both already.
        if (!l.opens_string)
            for (auto depth = stack.size() - 1; depth >= unit_depth; --depth)
            {
                auto const& g = file.at(stack[depth].group);
                if (g.kind == group_kind::quoted && !is_valid(g.close_token))
                {
                    close_down_to(depth, true);
                    break;
                }
            }

        if (has_block_colon)
        {
            settle_trailing_attributes();
            auto const block = append_new({.kind = group_kind::block, .token = block_colon});
            if (!has_content(l.first_child))
                report(diagnostic_kind::empty_block, block_colon);

            stack.push_back({.group = block});
            run_chain(l.first_child, chain_mode::statements);
            stack.remove_back();
        }
        else if (top_is_open_string())
            take_string_content(l.first_child);
        else if (is_list(file.at(stack.back().group).kind))
            run_chain(l.first_child, chain_mode::elements);
        else
        {
            // A continuation that continues again reads as nesting that is not there.
            if (mode == chain_mode::continuation)
                for (auto child = l.first_child; is_valid(child); child = file.at(child).next_sibling)
                    if (file.at(child).kind == line_kind::code && is_valid(first_significant_token(file.at(child))))
                    {
                        report(diagnostic_kind::nested_continuation, first_significant_token(file.at(child)));
                        break;
                    }
            run_chain(l.first_child, chain_mode::continuation);
        }

        if (stack.size() == unit_depth)
            settle_trailing_attributes();
    }

    [[nodiscard]] token_id first_significant_token(line const& l) const
    {
        for (auto t = l.tokens_begin(); t < l.tokens_end(); t = next(t))
            if (file.at(t).kind != token_kind::comment)
                return t;
        return token_id::none;
    }

    /// True if `t` closes the innermost thing that is open.
    [[nodiscard]] bool closes_innermost(token_id t) const
    {
        auto const& open = file.at(stack.back().group);
        switch (file.at(t).kind)
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
    [[nodiscard]] bool starts_with_infix(line const& l, token_id first_token) const
    {
        auto const& tok = file.at(first_token);
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
            auto const after = next(first_token);
            auto const has_next = after < l.tokens_end() && file.at(after).kind != token_kind::comment;
            return !has_next || !file.is_fused_left(after);
        }
        default:
            return false;
        }
    }

    void run_chain(line_id first_line, chain_mode mode)
    {
        auto const base = stack.size();
        auto const unit_depth = mode == chain_mode::statements ? base + 1 : base;
        auto const outer_floor = floor;
        auto owes_closer = false;

        for (auto i = first_line; is_valid(i); i = file.at(i).next_sibling)
        {
            auto const& l = file.at(i);
            if (l.kind != line_kind::code)
                continue;
            auto const first_token = first_significant_token(l);
            if (!is_valid(first_token))
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
                    next_is_fused = file.at(first_token).kind == token_kind::dot;
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
                report(diagnostic_kind::unattached_attribute, file.at(attribute).token);
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
