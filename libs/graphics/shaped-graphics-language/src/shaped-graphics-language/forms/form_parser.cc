#include "form_parser.hh"

#include <clean-core/common/assert.hh>

namespace
{
using namespace sgl;

/// The binary levels below the keyword form, loosest first.
/// All of them are flat runs of one loop but `arrow`, which nests to the right.
enum class level : u8
{
    connective,
    comparison,
    ascription,
    arrow,
    range,
    bit_like,
    add_like,
    mul_like,
    application,
};

/// Where an operator token belongs, read off its spelling alone.
enum class operator_class : u8
{
    assignment,
    comparison,
    range,
    bit_like,
    add_like,
    mul_like,
};

bool is_comparison(cc::string_view s)
{
    return s == "<" || s == "<=" || s == "==" || s == "!=" || s == ">=" || s == ">";
}

constexpr cc::string_view known_binary_operators[] = {"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "..<", "..="};

bool is_known_binary(cc::string_view s)
{
    for (auto const known : known_binary_operators)
        if (s == known)
            return true;
    return is_comparison(s);
}

bool is_known_operator(cc::string_view s)
{
    if (s == "=" || is_known_binary(s))
        return true;
    return s.size() > 1 && s.ends_with('=') && !s.starts_with("..")
        && is_known_binary(s.subview({.offset = 0, .size = s.size() - 1}))
        && !is_comparison(s.subview({.offset = 0, .size = s.size() - 1}));
}

/// Precedence comes from the first character, so an operator nobody declared still lands somewhere sensible.
operator_class classify(cc::string_view s)
{
    if (is_comparison(s))
        return operator_class::comparison;
    if (s.starts_with(".."))
        return operator_class::range;
    if (s.ends_with('='))
        return operator_class::assignment;
    switch (s[0])
    {
    case '*':
    case '/':
    case '%':
        return operator_class::mul_like;
    case '+':
    case '-':
        return operator_class::add_like;
    default:
        return operator_class::bit_like;
    }
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/// Digits of `base` with `'` between them; a separator is never first, last or doubled.
bool skip_digits(cc::string_view t, isize& i, int base)
{
    auto const is_digit_of_base = [&](char c)
    {
        if (base == 16)
            return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        return c >= '0' && c < '0' + base;
    };

    auto const start = i;
    while (i < t.size() && (is_digit_of_base(t[i]) || (t[i] == '\'' && i > start && t[i - 1] != '\'')))
        ++i;
    return i > start && t[i - 1] != '\'';
}

/// The grammar of an assembled number, sign excluded: prefix, digits, fraction, exponent, suffix.
/// Which bit widths exist is not decided here.
bool is_well_formed_number(cc::string_view t, bool& has_underscore)
{
    has_underscore = t.contains('_');
    auto i = isize(0);
    auto base = 10;
    if (t.starts_with("0x") || t.starts_with("0X"))
        base = 16;
    else if (t.starts_with("0b") || t.starts_with("0B"))
        base = 2;
    if (base != 10)
        i = 2;

    if (!skip_digits(t, i, base))
        return false;
    if (i < t.size() && t[i] == '.')
    {
        ++i;
        auto const before = i;
        if (!skip_digits(t, i, base) && i != before)
            return false;
    }

    auto const is_exponent = i < t.size() && (t[i] == 'p' || t[i] == 'P' || (base == 10 && (t[i] == 'e' || t[i] == 'E')));
    if (is_exponent)
    {
        ++i;
        if (i < t.size() && (t[i] == '+' || t[i] == '-'))
            ++i;
        if (!skip_digits(t, i, 10))
            return false;
    }

    if (i < t.size() && (t[i] == 'i' || t[i] == 'u' || t[i] == 'f'))
    {
        ++i;
        if (!skip_digits(t, i, 10))
            return false;
    }
    return i == t.size();
}

/// A position in one run of sibling groups, which is all the parser ever looks at.
struct cursor
{
    group_id at = group_id::none;
    /// The group the run stops before, `none` for the end of the chain.
    group_id end = group_id::none;
    /// The group consumed last, `none` at the start of the run.
    group_id previous = group_id::none;
};

struct form_parser
{
    parsed_file& file;
    cc::span<cc::string_view const> keywords;
    cursor c;
    /// Above zero inside a paren list, where a comma ends an element rather than separating a keyword form's expressions.
    int list_depth = 0;
    /// Groups whose attributes a form has already taken, so hoisting leaves them alone.
    cc::vector<group_id> claimed;

    // ---- groups -------------------------------------------------------------------------------------------------

    [[nodiscard]] bool at_end() const { return !is_valid(c.at) || c.at == c.end; }
    [[nodiscard]] group const& here() const { return file.at(c.at); }

    [[nodiscard]] group_id next_of(group_id g) const
    {
        auto const n = file.at(g).next_sibling;
        return n == c.end ? group_id::none : n;
    }

    void advance()
    {
        c.previous = c.at;
        c.at = file.at(c.at).next_sibling;
    }

    [[nodiscard]] bool is_token(group_id g, token_kind kind) const
    {
        return is_valid(g) && file.at(g).kind == group_kind::token && file.at(file.at(g).token).kind == kind;
    }

    [[nodiscard]] cc::string_view text_of(group_id g) const { return file.text_of(file.at(file.at(g).token).where); }

    [[nodiscard]] bool is_word(group_id g, cc::string_view word) const
    {
        return is_token(g, token_kind::symbol) && text_of(g) == word;
    }

    [[nodiscard]] bool is_keyword(group_id g) const
    {
        if (!is_token(g, token_kind::symbol))
            return false;
        auto const text = text_of(g);
        for (auto const k : keywords)
            if (text == k)
                return true;
        return false;
    }

    [[nodiscard]] bool is_word_operator(group_id g) const
    {
        return is_word(g, "and") || is_word(g, "or") || is_word(g, "not") || is_word(g, "as") || is_word(g, "in");
    }

    /// True if an operand can end with this group, which is what an operator has to touch to count as fused on its left.
    [[nodiscard]] bool ends_operand(group_id g) const
    {
        if (!is_valid(g))
            return false;
        auto const kind = file.at(g).kind;
        if (kind != group_kind::token)
            return kind != group_kind::block && kind != group_kind::statement;
        auto const token = file.at(file.at(g).token).kind;
        return token == token_kind::symbol || token == token_kind::wildcard;
    }

    [[nodiscard]] bool is_tight_left(group_id g) const { return file.at(g).is_fused_left && ends_operand(c.previous); }
    [[nodiscard]] bool is_tight_right(group_id g) const
    {
        auto const n = next_of(g);
        return is_valid(n) && file.at(n).is_fused_left;
    }

    [[nodiscard]] bool is_prefix_shaped(group_id g) const
    {
        return is_token(g, token_kind::op) && !is_tight_left(g) && is_tight_right(g);
    }
    [[nodiscard]] bool is_postfix_shaped(group_id g) const
    {
        return is_token(g, token_kind::op) && is_tight_left(g) && !is_tight_right(g);
    }

    void report(diagnostic_kind kind, source_span where)
    {
        file.diagnostics.push_back({.kind = kind, .level = default_severity_of(kind), .where = where});
    }

    // ---- forms --------------------------------------------------------------------------------------------------

    form_id make(form f)
    {
        file.forms.push_back(f);
        return form_id(i32(file.forms.size() - 1));
    }

    [[nodiscard]] source_span span_of_group(group_id g) const
    {
        auto const& gr = file.at(g);
        auto const start = file.at(gr.token).where;
        if (is_valid(gr.close_token))
            return cover(start, file.at(gr.close_token).where);
        auto result = start;
        for (auto child = gr.first_child; is_valid(child); child = file.at(child).next_sibling)
            result = cover(result, span_of_group(child));
        return result;
    }

    [[nodiscard]] static source_span cover(source_span a, source_span b)
    {
        if (a.empty())
            return b;
        if (b.empty())
            return a;
        auto const start = a.offset < b.offset ? a.offset : b.offset;
        auto const end = a.end() > b.end() ? a.end() : b.end();
        return {.offset = start, .length = end - start};
    }

    /// Appends `child` to `parent`, growing the parent's span over it.
    void add_child(form_id parent, form_id child, form_id& last)
    {
        if (is_valid(last))
            file.at(last).next_sibling = child;
        else
            file.at(parent).first_child = child;
        last = child;
        file.at(parent).where = cover(file.at(parent).where, file.at(child).where);
    }

    form_id leaf(form_kind kind, group_id g)
    {
        return make({.kind = kind, .where = span_of_group(g), .token = file.at(g).token});
    }

    form_id missing(source_span near)
    {
        report(diagnostic_kind::expected_expression, near);
        return make({.kind = form_kind::missing, .where = {.offset = near.end(), .length = 0}});
    }

    /// An operand directly after `:`, `->`, `as`, `in` or `=>` keeps the attributes written before it.
    /// That is how a return value is annotated; every other attribute belongs to the whole element.
    void claim_attributes_at_cursor(form_id form_after_parse, group_id group_before_parse)
    {
        if (!is_valid(group_before_parse))
            return;
        auto leads = false;
        for (auto a = file.at(group_before_parse).first_attribute; is_valid(a); a = file.at(a).next_sibling)
            leads = leads || !file.at(a).is_trailing;
        if (!leads)
            return;
        auto taken = cc::vector<group_id>();
        for (auto a = file.at(group_before_parse).first_attribute; is_valid(a); a = file.at(a).next_sibling)
            taken.push_back(a);
        auto const range = append_attributes(cc::span<group_id const>(taken));
        file.at(form_after_parse).first_attribute = range.first;
        file.at(form_after_parse).attribute_count = range.count;
        claimed.push_back(group_before_parse);
    }

    struct attribute_range
    {
        u32 first = 0;
        u32 count = 0;
    };

    /// The argument lists are parsed before anything is appended: a form's range must stay contiguous, and an
    /// argument may be a form with attributes of its own.
    attribute_range append_attributes(cc::span<group_id const> attributes)
    {
        auto lists = cc::vector<form_id>();
        for (auto const a : attributes)
        {
            auto const list = file.at(a).first_child;
            lists.push_back(is_valid(list) ? parse_list(list) : form_id::none);
        }

        auto const first = u32(file.form_attributes.size());
        file.form_attributes.push_back_range(attributes);
        file.form_attribute_arguments.push_back_range(cc::span<form_id const>(lists));
        return {.first = first, .count = u32(attributes.size())};
    }

    /// Gives `target` every unclaimed attribute written among `[first, end)`.
    void hoist_attributes(group_id first, group_id end, form_id target)
    {
        auto found = cc::vector<group_id>();
        for (auto g = first; is_valid(g) && g != end; g = file.at(g).next_sibling)
        {
            if (!is_valid(file.at(g).first_attribute))
                continue;
            auto is_claimed = false;
            for (auto const taken : claimed)
                is_claimed = is_claimed || taken == g;
            if (is_claimed)
                continue;

            for (auto a = file.at(g).first_attribute; is_valid(a); a = file.at(a).next_sibling)
                found.push_back(a);
        }
        if (found.empty())
            return;

        auto const range = append_attributes(cc::span<group_id const>(found));
        if (file.at(target).attribute_count == 0)
        {
            file.at(target).first_attribute = range.first;
            file.at(target).attribute_count = range.count;
        }
    }

    // ---- statements, elements, blocks --------------------------------------------------------------------------

    /// Parses the run `[first, end)` as one form; whatever the grammar leaves over is kept as `error` forms beside it.
    form_id parse_run(group_id first, group_id end)
    {
        auto const saved = c;
        c = {.at = first, .end = end};

        auto result = parse_sequence();
        if (!at_end())
        {
            auto const wrapper = make({.kind = form_kind::error});
            auto last = form_id::none;
            add_child(wrapper, result, last);
            while (!at_end())
            {
                report(diagnostic_kind::unexpected_token, span_of_group(c.at));
                add_child(wrapper, make({.kind = form_kind::error, .where = span_of_group(c.at)}), last);
                advance();
            }
            result = wrapper;
        }

        hoist_attributes(first, end, result);
        c = saved;
        return result;
    }

    form_id parse_block(group_id block_group)
    {
        auto const saved_depth = list_depth;
        list_depth = 0;

        auto const& g = file.at(block_group);
        auto const result
            = make({.kind = form_kind::block, .where = is_valid(g.token) ? file.at(g.token).where : source_span{}});
        auto last = form_id::none;
        for (auto statement = g.first_child; is_valid(statement); statement = file.at(statement).next_sibling)
            if (is_valid(file.at(statement).first_child))
                add_child(result, parse_run(file.at(statement).first_child, group_id::none), last);

        list_depth = saved_depth;
        return result;
    }

    form_id parse_list(group_id list_group)
    {
        auto const& g = file.at(list_group);
        auto const kind = g.kind == group_kind::round  ? form_kind::round_list
                        : g.kind == group_kind::square ? form_kind::square_list
                                                       : form_kind::curly_list;
        auto const result = make({.kind = kind, .where = span_of_group(list_group), .token = g.token});

        ++list_depth;
        auto last = form_id::none;
        auto start = g.first_child;
        while (is_valid(start))
        {
            // An element ends at a comma, and at the first group of the next element line.
            auto end = start;
            while (is_valid(end) && !is_token(end, token_kind::comma) && (end == start || !file.at(end).starts_line))
                end = file.at(end).next_sibling;

            if (end != start)
                add_child(result, parse_run(start, end), last);
            start = is_token(end, token_kind::comma) ? file.at(end).next_sibling : end;
        }
        --list_depth;

        file.at(result).where = span_of_group(list_group);
        return result;
    }

    // ---- the ladder --------------------------------------------------------------------------------------------

    form_id parse_sequence()
    {
        auto const first = parse_assignment();
        if (!is_token(c.at, token_kind::semicolon) || at_end())
            return first;

        auto const result = make({.kind = form_kind::sequence});
        auto last = form_id::none;
        add_child(result, first, last);
        while (!at_end() && is_token(c.at, token_kind::semicolon))
        {
            if (list_depth > 0)
                report(diagnostic_kind::semicolon_in_parens, span_of_group(c.at));
            advance();
            if (!at_end())
                add_child(result, parse_assignment(), last);
        }
        return result;
    }

    [[nodiscard]] bool at_assignment() const
    {
        return !at_end() && is_token(c.at, token_kind::op) && !is_prefix_shaped(c.at) && !is_postfix_shaped(c.at)
            && classify(text_of(c.at)) == operator_class::assignment;
    }

    form_id binary(form_id left, group_id operator_group, form_id right)
    {
        auto const result = make({.kind = form_kind::operator_run});
        auto last = form_id::none;
        add_child(result, left, last);
        add_child(result, leaf(form_kind::op, operator_group), last);
        add_child(result, right, last);
        return result;
    }

    form_id parse_assignment()
    {
        auto const left = parse_computes_as();
        if (!at_assignment())
            return left;

        auto const operator_group = c.at;
        check_operator(operator_group);
        advance();
        auto const right = at_end() ? missing(span_of_group(operator_group)) : parse_assignment();
        return binary(left, operator_group, right);
    }

    form_id parse_computes_as()
    {
        auto const left = parse_head();
        if (at_end() || !is_token(c.at, token_kind::double_arrow))
            return left;

        auto const operator_group = c.at;
        check_operator(operator_group);
        advance();
        auto const operand_group = c.at;
        auto right = form_id::none;
        if (at_end())
            right = missing(span_of_group(operator_group));
        else if (here().kind == group_kind::block)
        {
            right = parse_block(c.at);
            advance();
        }
        else
        {
            right = parse_computes_as();
            claim_attributes_at_cursor(right, operand_group);
        }
        return binary(left, operator_group, right);
    }

    /// A keyword form, or an expression — which becomes a keyword form without keywords when a block hangs off it.
    form_id parse_head()
    {
        if (!at_end() && is_keyword(c.at))
            return parse_keyword_form();

        auto const expression = parse_level(level::connective);
        if (at_end() || here().kind != group_kind::block)
            return expression;

        auto const result = make({.kind = form_kind::keyword_form});
        auto last = form_id::none;
        add_child(result, expression, last);
        add_child(result, parse_block(c.at), last);
        advance();
        return result;
    }

    [[nodiscard]] bool ends_keyword_arguments() const
    {
        if (at_end() || here().kind == group_kind::block)
            return true;
        return is_token(c.at, token_kind::double_arrow) || is_token(c.at, token_kind::semicolon) || at_assignment()
            || (list_depth > 0 && is_token(c.at, token_kind::comma));
    }

    form_id parse_keyword_form()
    {
        auto const result = make({.kind = form_kind::keyword_form});
        auto last = form_id::none;
        while (!at_end() && is_keyword(c.at))
        {
            add_child(result, leaf(form_kind::keyword, c.at), last);
            advance();
        }

        while (!ends_keyword_arguments())
        {
            add_child(result, parse_level(level::connective), last);
            if (at_end() || list_depth > 0 || !is_token(c.at, token_kind::comma))
                break;
            advance();
        }

        if (!at_end() && here().kind == group_kind::block)
        {
            add_child(result, parse_block(c.at), last);
            advance();
        }
        return result;
    }

    /// The level an infix operator at the cursor belongs to, or nothing if the cursor is not on one.
    [[nodiscard]] bool infix_at(level wanted) const
    {
        if (at_end() || here().kind != group_kind::token)
            return false;

        switch (file.at(here().token).kind)
        {
        case token_kind::colon:
            return wanted == level::ascription;
        case token_kind::arrow:
            return wanted == level::arrow;
        case token_kind::symbol:
            if (is_word(c.at, "and") || is_word(c.at, "or"))
                return wanted == level::connective;
            return wanted == level::ascription && (is_word(c.at, "as") || is_word(c.at, "in"));
        case token_kind::op:
            if (is_prefix_shaped(c.at) || is_postfix_shaped(c.at))
                return false;
            switch (classify(text_of(c.at)))
            {
            case operator_class::assignment:
                return false;
            case operator_class::comparison:
                return wanted == level::comparison;
            case operator_class::range:
                return wanted == level::range;
            case operator_class::bit_like:
                return wanted == level::bit_like;
            case operator_class::add_like:
                return wanted == level::add_like;
            case operator_class::mul_like:
                return wanted == level::mul_like;
            }
            return false;
        default:
            return false;
        }
    }

    /// What an operator token owes beyond its place in the ladder: a known spelling, and spaces around it.
    void check_operator(group_id g)
    {
        // `->` and `=>` are tokens of their own and owe the same spaces; `:` and the word operators owe none.
        if (is_token(g, token_kind::arrow) || is_token(g, token_kind::double_arrow))
        {
            if (is_tight_left(g) && is_tight_right(g))
                report(diagnostic_kind::operator_needs_spaces, span_of_group(g));
            return;
        }
        if (!is_token(g, token_kind::op))
            return;
        auto const text = text_of(g);
        if (text == "..")
            report(diagnostic_kind::bare_range, span_of_group(g));
        else if (text == "!" || text == "?")
            report(diagnostic_kind::reserved_operator, span_of_group(g));
        else if (!is_known_operator(text))
            report(diagnostic_kind::unknown_operator, span_of_group(g));
        // A range may touch its operands: `0..<4` is how everyone writes one.
        if (is_tight_left(g) && is_tight_right(g) && !text.starts_with(".."))
            report(diagnostic_kind::operator_needs_spaces, span_of_group(g));
    }

    form_id parse_level(level l)
    {
        if (l == level::application)
            return parse_application();
        if (l == level::arrow)
            return parse_arrow();

        auto const tighter = level(u8(l) + 1);
        auto const operand = [&]
        {
            if (l == level::connective && !at_end() && is_word(c.at, "not"))
            {
                auto const result
                    = make({.kind = form_kind::prefix_operator, .where = span_of_group(c.at), .token = here().token});
                auto const word = c.at;
                advance();
                auto last = form_id::none;
                add_child(result, at_end() ? missing(span_of_group(word)) : parse_level(tighter), last);
                return result;
            }
            return parse_level(tighter);
        };

        auto const first = operand();
        if (!infix_at(l))
            return first;

        auto const result = make({.kind = form_kind::operator_run});
        auto last = form_id::none;
        add_child(result, first, last);
        while (infix_at(l))
        {
            auto const operator_group = c.at;
            check_operator(operator_group);
            add_child(result, leaf(form_kind::op, operator_group), last);
            advance();

            auto const operand_group = c.at;
            auto const right = at_end() ? missing(span_of_group(operator_group)) : operand();
            if (l == level::ascription)
                claim_attributes_at_cursor(right, operand_group);
            add_child(result, right, last);
        }

        judge_run(result, l);
        return result;
    }

    /// `->` nests to the right, so `a -> b -> c` is a function returning a function.
    form_id parse_arrow()
    {
        auto const left = parse_level(level::range);
        if (!infix_at(level::arrow))
            return left;

        auto const operator_group = c.at;
        check_operator(operator_group);
        advance();
        auto const operand_group = c.at;
        auto const right = at_end() ? missing(span_of_group(operator_group)) : parse_arrow();
        claim_attributes_at_cursor(right, operand_group);
        return binary(left, operator_group, right);
    }

    /// Runs parse whatever they hold; what the language forbids among equals is said here.
    void judge_run(form_id run, level l)
    {
        auto operators = cc::vector<cc::string_view>();
        auto operand_count = 0;
        auto index = 0;
        for (auto child = file.at(run).first_child; is_valid(child); child = file.at(child).next_sibling, ++index)
        {
            auto const& f = file.at(child);
            if (f.kind == form_kind::op && index % 2 == 1)
                operators.push_back(file.text_of(file.at(f.token).where));
            else
                ++operand_count;
        }

        auto const where = file.at(run).where;
        if (l == level::connective)
        {
            auto seen = 0;
            for (auto child = file.at(run).first_child; is_valid(child); child = file.at(child).next_sibling)
            {
                auto const& f = file.at(child);
                if (f.kind == form_kind::op)
                    continue;
                ++seen;
                auto const is_not = f.kind == form_kind::prefix_operator && file.text_of(file.at(f.token).where) == "not";
                if (is_not && seen < operand_count)
                    report(diagnostic_kind::misplaced_not, f.where);
            }
        }

        if (l == level::connective || l == level::bit_like)
        {
            for (auto const op : operators)
                if (op != operators[0])
                {
                    report(diagnostic_kind::mixed_operators, where);
                    break;
                }
        }
        else if (l == level::range && operators.size() > 1)
            report(diagnostic_kind::chained_range, where);
        else if (l == level::comparison && operators.size() > 1)
        {
            // A chain may only ever go one way, `==` fits into either, and `!=` fits into none.
            auto goes_up = false;
            auto goes_down = false;
            auto has_unequal = false;
            for (auto const op : operators)
            {
                goes_up = goes_up || op == "<" || op == "<=";
                goes_down = goes_down || op == ">" || op == ">=";
                has_unequal = has_unequal || op == "!=";
            }
            if ((goes_up && goes_down) || has_unequal)
                report(diagnostic_kind::non_monotone_comparison, where);
        }
    }

    [[nodiscard]] bool starts_operand() const
    {
        if (at_end())
            return false;
        auto const& g = here();
        if (g.kind == group_kind::block || g.kind == group_kind::statement)
            return false;
        if (g.kind != group_kind::token)
            return true;

        switch (file.at(g.token).kind)
        {
        case token_kind::symbol:
            return !is_word_operator(c.at);
        case token_kind::wildcard:
        case token_kind::error:
            return true;
        case token_kind::dot:
            return is_token(next_of(c.at), token_kind::symbol) && file.at(next_of(c.at)).is_fused_left;
        case token_kind::op:
            return is_prefix_shaped(c.at);
        default:
            return false;
        }
    }

    form_id parse_application()
    {
        auto const head = parse_prefix();
        if (!starts_operand())
            return head;

        auto const result = make({.kind = form_kind::application});
        auto last = form_id::none;
        add_child(result, head, last);
        while (starts_operand())
            add_child(result, parse_prefix(), last);
        return result;
    }

    form_id parse_prefix()
    {
        if (at_end() || !is_prefix_shaped(c.at))
            return parse_postfix();

        auto const operator_group = c.at;
        auto const text = text_of(operator_group);
        if (text == "!" || text == "?")
            report(diagnostic_kind::reserved_operator, span_of_group(operator_group));
        // `..x` is the splat; whether it stands where a splat may stand is for the phase that knows what a list is.
        else if (text != "-" && text != "+" && text != "~" && text != "..")
            report(diagnostic_kind::unknown_operator, span_of_group(operator_group));
        advance();
        auto const operand = parse_prefix();

        // A sign directly on a number is part of the number, which is what makes `-3` a literal.
        if ((text == "-" || text == "+") && file.at(operand).kind == form_kind::number)
        {
            file.at(operand).where = cover(span_of_group(operator_group), file.at(operand).where);
            return operand;
        }

        auto const result = make({.kind = form_kind::prefix_operator,
                                  .where = span_of_group(operator_group),
                                  .token = file.at(operator_group).token});
        auto last = form_id::none;
        add_child(result, operand, last);
        return result;
    }

    form_id parse_postfix()
    {
        auto result = parse_atom();
        while (!at_end() && is_tight_left(c.at))
        {
            auto const& g = here();
            // `a::b` is read as `a.b`, and says so.
            auto const is_accessor = is_token(c.at, token_kind::dot) || is_token(c.at, token_kind::double_colon);
            auto const is_member
                = is_accessor && is_valid(next_of(c.at)) && file.at(next_of(c.at)).is_fused_left
               && (is_token(next_of(c.at), token_kind::symbol) || is_token(next_of(c.at), token_kind::wildcard));
            if (g.kind == group_kind::round || g.kind == group_kind::square || g.kind == group_kind::curly)
            {
                auto const call = make({.kind = form_kind::call});
                auto last = form_id::none;
                add_child(call, result, last);
                add_child(call, parse_list(c.at), last);
                advance();
                result = call;
            }
            else if (is_member)
            {
                if (is_token(c.at, token_kind::double_colon))
                    report(diagnostic_kind::double_colon, span_of_group(c.at));
                advance();
                auto const member
                    = make({.kind = form_kind::member, .where = span_of_group(c.at), .token = here().token});
                auto last = form_id::none;
                add_child(member, result, last);
                advance();
                result = member;
            }
            else if (is_postfix_shaped(c.at))
            {
                report(diagnostic_kind::reserved_operator, span_of_group(c.at));
                auto const postfix
                    = make({.kind = form_kind::postfix_operator, .where = span_of_group(c.at), .token = g.token});
                auto last = form_id::none;
                add_child(postfix, result, last);
                advance();
                result = postfix;
            }
            else
                break;
        }
        return result;
    }

    [[nodiscard]] bool is_fused_symbol(group_id g) const
    {
        return is_valid(g) && is_token(g, token_kind::symbol) && file.at(g).is_fused_left;
    }

    [[nodiscard]] bool is_digit_led(group_id g) const { return is_digit(text_of(g)[0]); }

    /// A number is assembled from fused tokens, since the tokenizer has no idea what a number is.
    form_id parse_number()
    {
        auto const first = c.at;
        auto where = span_of_group(first);
        auto last_symbol = first;
        advance();

        auto const dot = c.at;
        if (!at_end() && is_token(dot, token_kind::dot) && file.at(dot).is_fused_left)
        {
            auto const after = next_of(dot);
            if (is_fused_symbol(after) && is_digit_led(after))
            {
                where = cover(where, span_of_group(after));
                last_symbol = after;
                advance();
                advance();
            }
            else if (!is_fused_symbol(after) && !is_token(after, token_kind::wildcard))
            {
                // `1.` is a number; `1.max` is a member of the number `1`.
                where = cover(where, span_of_group(dot));
                last_symbol = group_id::none;
                advance();
            }
        }

        if (is_valid(last_symbol) && !at_end())
        {
            auto const text = text_of(last_symbol);
            auto const is_hex = text_of(first).starts_with("0x") || text_of(first).starts_with("0X");
            auto const tail = text[text.size() - 1];
            auto const opens_exponent = tail == 'p' || tail == 'P' || (!is_hex && (tail == 'e' || tail == 'E'));
            auto const sign = c.at;
            auto const digits = next_of(sign);
            if (opens_exponent && is_token(sign, token_kind::op) && file.at(sign).is_fused_left
                && (text_of(sign) == "-" || text_of(sign) == "+") && is_fused_symbol(digits) && is_digit_led(digits))
            {
                where = cover(where, span_of_group(digits));
                advance();
                advance();
            }
        }

        auto has_underscore = false;
        if (!is_well_formed_number(file.text_of(where), has_underscore))
            report(has_underscore ? diagnostic_kind::underscore_in_number : diagnostic_kind::malformed_number, where);
        return make({.kind = form_kind::number, .where = where, .token = file.at(first).token});
    }

    form_id parse_atom()
    {
        if (at_end())
            return missing(is_valid(c.previous) ? span_of_group(c.previous) : source_span{});

        auto const g = c.at;
        auto const kind = here().kind;
        if (kind == group_kind::round || kind == group_kind::square || kind == group_kind::curly)
        {
            auto const result = parse_list(g);
            advance();
            return result;
        }
        if (kind == group_kind::quoted)
        {
            advance();
            return make({.kind = form_kind::quoted, .where = span_of_group(g), .token = file.at(g).token});
        }
        if (kind != group_kind::token)
            return missing(span_of_group(g));

        switch (file.at(here().token).kind)
        {
        case token_kind::wildcard:
            advance();
            return leaf(form_kind::wildcard, g);
        case token_kind::error:
            advance();
            return leaf(form_kind::error, g);

        case token_kind::symbol:
        {
            if (is_keyword(g))
                return parse_keyword_form();
            auto const text = text_of(g);
            if (is_digit(text[0]))
                return parse_number();
            advance();
            return leaf(text[0] == '#' ? form_kind::hash_literal : form_kind::identifier, g);
        }

        case token_kind::dot:
            if (is_fused_symbol(next_of(g)) || (is_valid(next_of(g)) && is_token(next_of(g), token_kind::wildcard)))
            {
                advance();
                auto const name = c.at;
                advance();
                return make({.kind = form_kind::leading_dot,
                             .where = cover(span_of_group(g), span_of_group(name)),
                             .token = file.at(name).token});
            }
            break;

        default:
            break;
        }

        // Nothing an operand can start with: say so and leave the token for whoever can use it.
        return missing(is_valid(c.previous) ? span_of_group(c.previous) : span_of_group(g));
    }
};

constexpr cc::string_view sgl_keywords[] = {
    "fun",    "let",      "mut",    "out",  "struct", "enum",  "binding",  "sampler", "pipeline",
    "const",  "use",      "module", "type", "if",     "else",  "for",      "while",   "loop",
    "return", "continue", "break",  "case", "assert", "print", "notation", "yield",   "test",
};
} // namespace

cc::span<cc::string_view const> sgl::default_keywords()
{
    return cc::span<cc::string_view const>(sgl_keywords);
}

void sgl::parse_forms(parsed_file& file, cc::span<cc::string_view const> keywords)
{
    CC_ASSERT(file.forms.empty(), "a file's form tree is built once");
    CC_ASSERT(is_valid(file.root_block), "the file must be grouped first");

    auto parser = form_parser{.file = file, .keywords = keywords};
    file.root_form = parser.parse_block(file.root_block);
}
