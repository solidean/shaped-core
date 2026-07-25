#include "lint_corpus.hh"

#include <babel-serializer/data/markdown.hh>
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
    while (i < n && !is_space(info[i])) // the language word, already known to be "cpp"
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
            (negated ? out.forbid : out.expect).push_back(cc::string(id));
            i = close + 1;
            continue;
        }

        // fix="…", with \" and \\ escapes
        constexpr auto k_fix = cc::string_view("fix=");
        if (n - i >= k_fix.size() && info.subview({.start = i, .end = i + k_fix.size()}) == k_fix)
        {
            i += k_fix.size();
            if (i >= n || info[i] != '"')
            {
                what = cc::string("`fix=` must be followed by a double-quoted string");
                return false;
            }
            ++i;

            auto value = cc::string();
            while (i < n && info[i] != '"')
            {
                if (info[i] == '\\' && i + 1 < n)
                    ++i;
                value += info[i];
                ++i;
            }
            if (i >= n)
            {
                what = cc::string("unterminated `fix=\"…\"`");
                return false;
            }
            ++i; // the closing quote

            if (out.fix.has_value())
            {
                what = cc::string("a block may carry at most one `fix=`");
                return false;
            }
            out.fix = cc::move(value);
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

/// Walk a parsed corpus file in document order, turning every annotated `cpp` block into a case and
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
        if (!n.is_code_block() || language_of(n.info()) != "cpp")
            continue;

        auto c = lint_corpus_case{.title = heading, .line = n.line(), .source = cc::string(n.text())};

        auto what = cc::string();
        if (!parse_annotations(n.info(), c, what))
            return cc::error(cc::format("{}:{}: bad corpus annotation: {}", relative_path, n.line(), what));

        if (c.expect.empty() && c.forbid.empty())
        {
            ++group.skipped; // an illustrative block, not a case
            continue;
        }
        if (c.fix.has_value() && c.expect.empty())
            return cc::error(cc::format("{}:{}: `fix=` needs a `[rule-id]` that produces it", relative_path, n.line()));

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

    auto stream = adapter.value().stream();
    auto size = stream.size();
    CC_RETURN_IF_ERROR(size);

    // TEMPORARY: slurped rather than streamed. `file_read_stream_adapter::stream()` hands out a
    // cc::seekable_read_stream, and clean-core offers no narrowing conversion from that to the plain
    // cc::read_stream every babel reader takes — so a file cannot reach the streaming path at all today.
    // Corpus files are a few KB, so this costs nothing; the real fix is that conversion in clean-core.
    auto bytes = cc::vector<cc::byte>::create_defaulted(size.value());
    CC_RETURN_IF_ERROR(stream.read_exact(bytes));

    return parse_lint_corpus(cc::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size()), relative_path);
}
} // namespace scl
