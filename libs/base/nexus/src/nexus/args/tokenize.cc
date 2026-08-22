#include "tokenize.hh"

#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>

namespace
{
using nx::isize;

/// Whether a token has been started, as distinct from whether it has any characters.
/// `""` is a real, empty token, and losing that would make an intentionally empty argument unwritable.
struct token_builder
{
    cc::string text;
    bool started = false;

    void begin() { started = true; }
    void put(char c)
    {
        started = true;
        text += c;
    }

    void flush(cc::vector<cc::string>& out)
    {
        if (started)
            out.push_back(cc::move(text));

        text = cc::string();
        started = false;
    }
};

char escaped_char(char c)
{
    switch (c)
    {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case 'r':
        return '\r';
    case '0':
        return '\0';
    default:
        return c; // \" and \\ land here, and so does anything else: a backslash before it is just itself
    }
}
} // namespace

cc::vector<cc::string> nx::args_tokenize(cc::string_view text)
{
    auto out = cc::vector<cc::string>();
    auto current = token_builder();

    for (auto i = isize(0); i < text.size(); ++i)
    {
        auto const c = text[i];

        if (cc::is_space(c))
        {
            current.flush(out);
            continue;
        }

        // Only where a token would start: a '#' inside one is an ordinary character, so `--tag=#1` works.
        if (c == '#' && !current.started)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;

            continue;
        }

        if (c == '"')
        {
            current.begin();
            ++i;
            while (i < text.size() && text[i] != '"')
            {
                if (text[i] == '\\' && i + 1 < text.size())
                {
                    ++i;
                    current.put(escaped_char(text[i]));
                }
                else
                    current.put(text[i]);

                ++i;
            }

            continue;
        }

        if (c == '\'')
        {
            // Nothing is an escape in here, which is what lets a Windows path be pasted in unchanged.
            current.begin();
            ++i;
            while (i < text.size() && text[i] != '\'')
            {
                current.put(text[i]);
                ++i;
            }

            continue;
        }

        current.put(c);
    }

    current.flush(out);
    return out;
}

namespace
{
cc::result<cc::string> read_whole_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
        return cc::error(cc::format("could not open '{}'", path));

    // The stream's window points into the adapter, so the adapter has to outlive it — hence both locals.
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    if (bytes.has_error())
        return cc::error(cc::format("could not read '{}'", path));

    auto const& data = bytes.value();
    return cc::string(reinterpret_cast<char const*>(data.data()), data.size());
}

void splice_into(cc::span<cc::string const> tokens, int depth, int max_depth, nx::args_splice_result& result);

void splice_token(cc::string_view token, int depth, int max_depth, nx::args_splice_result& result)
{
    // `@@rest` is how an argument that genuinely begins with '@' is written.
    if (token.starts_with("@@"))
    {
        result.tokens.push_back(cc::string(token.subview(1)));
        return;
    }

    if (!token.starts_with('@') || token.size() == 1)
    {
        result.tokens.push_back(cc::string(token));
        return;
    }

    auto const path = token.subview(1);

    if (depth >= max_depth)
    {
        result.errors.push_back(cc::format("response file '{}' is nested more than {} deep", path, max_depth));
        return;
    }

    auto const contents = read_whole_file(path);
    if (contents.has_error())
    {
        result.errors.push_back(contents.error().to_string());
        return;
    }

    splice_into(nx::args_tokenize(contents.value()), depth + 1, max_depth, result);
}

void splice_into(cc::span<cc::string const> tokens, int depth, int max_depth, nx::args_splice_result& result)
{
    for (auto i = isize(0); i < tokens.size(); ++i)
    {
        // Past a bare `--` the tokens belong to whoever receives the tail, and rewriting them here would
        // silently change what that program is handed.
        if (tokens[i] == "--")
        {
            for (auto j = i; j < tokens.size(); ++j)
                result.tokens.push_back(tokens[j]);

            return;
        }

        splice_token(tokens[i], depth, max_depth, result);
    }
}
} // namespace

nx::args_splice_result nx::args_splice_response_files(cc::span<cc::string_view const> tokens, int max_depth)
{
    auto owned = cc::vector<cc::string>();
    for (auto const& t : tokens)
        owned.push_back(cc::string(t));

    auto result = args_splice_result();
    splice_into(owned, 0, max_depth, result);
    return result;
}
