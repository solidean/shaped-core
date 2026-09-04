#include "lint_corpus.hh"

#include <babel-data/data/markdown.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>

namespace scl
{
namespace
{
namespace md = babel::markdown;

bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

/// The annotations that follow the language word in a fence info string, parsed into `out`.
/// `false` on a malformed annotation; `what` then says why.
bool parse_annotations(cc::string_view info, lint_corpus_case& out, cc::string& what)
{
    auto i = isize(0);
    auto const n = info.size();

    auto skip_spaces = [&]
    {
        while (i < n && is_space(info[i]))
            ++i;
    };

    skip_spaces();
    while (i < n && !is_space(info[i])) // the language word, already known to be a lintable one
        ++i;

    while (true)
    {
        skip_spaces();
        if (i >= n)
            return true;

        // ~[rule-id] / [rule-id]
        auto const negated = info[i] == '~';
        if (negated || info[i] == '[')
        {
            if (negated && (i + 1 >= n || info[i + 1] != '['))
            {
                what = cc::format("`~` must be followed by `[rule-id]` at offset {}", i);
                return false;
            }
            auto const start = i + (negated ? 2 : 1);
            auto close = start;
            while (close < n && info[close] != ']')
                ++close;
            if (close >= n)
            {
                what = cc::format("unterminated `[` at offset {}", i);
                return false;
            }
            auto const id = info.subview({.start = start, .end = close});
            if (id.empty())
            {
                what = cc::format("empty rule id at offset {}", i);
                return false;
            }
            out.expect.push_back({.rule_id = cc::string(id), .negated = negated});
            i = close + 1;
            continue;
        }

        // key="…", with \" and \\ escapes.
        // `fix=` / `hint=` pin a replacement text and bind identically,
        // differing only in which of the rule's two rewrite channels they read; `path=` and `config=`
        // describe the block itself and bind to no rule at all.
        auto matched_keyword = cc::string_view();
        for (auto const kw :
             {cc::string_view("fix="), cc::string_view("hint="), cc::string_view("path="), cc::string_view("config=")})
            if (n - i >= kw.size() && info.subview({.start = i, .end = i + kw.size()}) == kw)
                matched_keyword = kw;

        if (!matched_keyword.empty())
        {
            auto const label = matched_keyword.subview({.start = 0, .end = matched_keyword.size() - 1});
            i += matched_keyword.size();
            if (i >= n || info[i] != '"')
            {
                what = cc::format("`{}=` must be followed by a double-quoted string", label);
                return false;
            }
            ++i;

            // `\n` / `\t` / `\r` become the real character — a replacement that splices in a whole line
            // has to be spellable on the one line a fence info string gets.
            // Any other `\x` is just `x`,
            // which is what makes `\"` and `\\` work.
            auto value = cc::string();
            while (i < n && info[i] != '"')
            {
                if (info[i] == '\\' && i + 1 < n)
                {
                    ++i;
                    if (info[i] == 'n')
                        value += '\n';
                    else if (info[i] == 't')
                        value += '\t';
                    else if (info[i] == 'r')
                        value += '\r';
                    else
                        value += info[i];
                    ++i;
                    continue;
                }
                value += info[i];
                ++i;
            }
            if (i >= n)
            {
                what = cc::format("unterminated `{}=\"…\"`", label);
                return false;
            }
            ++i; // the closing quote

            if (label == "path")
            {
                if (!out.path.empty())
                {
                    what = cc::string("a block has only one `path=`");
                    return false;
                }
                if (value.empty())
                {
                    what = cc::string("`path=` must name a file");
                    return false;
                }
                out.path = cc::move(value);
                continue;
            }

            if (label == "config")
            {
                if (!out.config.empty())
                {
                    what = cc::string("a block has only one `config=`");
                    return false;
                }
                if (value.empty())
                {
                    what = cc::string("`config=` must carry a config");
                    return false;
                }
                out.config = cc::move(value);
                continue;
            }

            // A fix or hint belongs to the rule in front of it — that is what associates it with a rule at
            // all, and what lets two rules on one block each pin their own rewrite.
            if (out.expect.empty())
            {
                what = cc::format("`{}=` must follow a `[rule-id]` that produces it", label);
                return false;
            }
            if (out.expect.back().negated)
            {
                what = cc::format("`~[{}]` must not fire, so it cannot carry a `{}=`", out.expect.back().rule_id, label);
                return false;
            }
            if (label == "fix")
                out.expect.back().fixes.push_back(cc::move(value));
            else
                out.expect.back().hints.push_back(cc::move(value));
            continue;
        }

        auto stop = i;
        while (stop < n && !is_space(info[stop]))
            ++stop;
        what = cc::format("unknown annotation `{}`", info.subview({.start = i, .end = stop}));
        return false;
    }
}

/// The language word of a fence info string — everything before the first blank.
cc::string_view language_of(cc::string_view info)
{
    auto e = isize(0);
    while (e < info.size() && !is_space(info[e]))
        ++e;
    return info.subview({.start = 0, .end = e});
}

/// Whether a fence's language word marks a block the linter can lint.
///
/// The word only says what the block IS; what it is linted AS comes from `path=`, since that is what the
/// engine reads.
/// So `py` and `md` open the door for a Python or markdown case, and a `text` or `json`
/// block stays illustration.
bool is_lintable_language(cc::string_view word)
{
    return word == "cpp" || word == "py" || word == "python" || word == "md" || word == "markdown";
}

/// Walk a parsed corpus file in document order, turning every annotated block into a case and
/// carrying the nearest preceding heading along as its title.
cc::result<lint_corpus_group> build_group(md::document const& doc, cc::string_view relative_path)
{
    auto group = lint_corpus_group{.path = cc::string(relative_path)};
    auto heading = cc::string("(no heading)");

    for (auto i = 0; i < doc.node_count(); ++i)
    {
        auto const n = doc.node_at(i);

        if (n.is_heading())
        {
            heading = cc::string(n.text());
            continue;
        }
        if (!n.is_code_block() || !is_lintable_language(language_of(n.info())))
            continue;

        auto c = lint_corpus_case{.title = heading, .line = n.line(), .source = cc::string(n.text())};

        auto what = cc::string();
        if (!parse_annotations(n.info(), c, what))
            return cc::error(cc::format("{}:{}: bad corpus annotation: {}", relative_path, n.line(), what));

        if (c.expect.empty())
        {
            ++group.skipped; // an illustrative block, not a case
            continue;
        }

        group.cases.push_back(cc::move(c));
    }

    return cc::move(group);
}
} // namespace

cc::result<lint_corpus_group> parse_lint_corpus(cc::string_view text, cc::string_view relative_path)
{
    auto doc = md::read(text);
    CC_RETURN_IF_ERROR(doc);
    return build_group(doc.value(), relative_path);
}

cc::result<lint_corpus_group> load_lint_corpus(cc::string_view file_path, cc::string_view relative_path)
{
    // The adapter owns the buffer the stream reads through, so it must outlive the stream.
    auto adapter = cc::file_read_stream_adapter::open(file_path);
    CC_RETURN_IF_ERROR(adapter);

    // The adapter narrows straight to the plain read_stream every babel reader takes, so the file feeds the
    // markdown parser through its buffer — no slurp.
    cc::read_stream stream = adapter.value();
    auto doc = md::read(stream);
    CC_RETURN_IF_ERROR(doc);

    return build_group(doc.value(), relative_path);
}
} // namespace scl
