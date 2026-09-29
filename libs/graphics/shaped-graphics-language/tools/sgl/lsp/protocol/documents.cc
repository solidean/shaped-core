#include "documents.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/uri.hh>

using namespace cc::primitive_defines;

lsp::document const* lsp::snapshot::find(cc::string_view uri) const
{
    auto const* d = _documents.get_ptr(uri);
    return d != nullptr ? d->get() : nullptr;
}

cc::shared_ptr<lsp::document> lsp::snapshot::share(cc::string_view uri) const
{
    auto const* d = _documents.get_ptr(uri);
    return d != nullptr ? *d : cc::shared_ptr<document>();
}

i32 lsp::snapshot::version_of(cc::string_view uri) const
{
    auto const* d = find(uri);
    return d != nullptr ? d->version : -1;
}

cc::vector<cc::string> lsp::snapshot::uris() const
{
    auto out = cc::vector<cc::string>();
    for (auto [uri, d] : _documents)
        out.push_back(uri);
    return out;
}

void lsp::workspace::open(text_document_item const& item)
{
    auto d = cc::make_shared<document>();
    d->uri = item.uri;
    d->version = item.version;
    d->text = item.text;
    d->index = text_index(d->text);
    _current._documents[item.uri] = cc::move(d);
}

bool lsp::workspace::change(did_change_params const& params, position_encoding e)
{
    auto const* old = _current.find(params.uri);
    if (old == nullptr)
    {
        CC_LOG_WARNING("didChange for {}, which is not open", params.uri);
        return false;
    }

    auto text = old->text;
    auto index = old->index;
    for (auto const& c : params.content_changes)
    {
        if (!c.range.has_value())
            text = c.text;
        else
        {
            auto const start = index.offset_of(text, c.range.value().start, e);
            auto end = index.offset_of(text, c.range.value().end, e);
            if (end < start)
                end = start;
            text.replace({.start = start, .end = end}, c.text);
        }
        index = text_index(text);
    }

    auto d = cc::make_shared<document>();
    d->uri = params.uri;
    d->version = params.version;
    d->text = cc::move(text);
    d->index = cc::move(index);
    _current._documents[params.uri] = cc::move(d);
    return true;
}

void lsp::workspace::close(cc::string_view uri)
{
    _current._documents.erase(uri);
}

cc::optional<cc::string> lsp::path_of_uri(cc::string_view uri)
{
    auto const parsed = cc::uri_view::parse(uri);
    if (!parsed.has_value())
        return cc::nullopt;
    constexpr auto file = cc::string_view("file");
    auto const scheme = parsed.value().scheme();
    if (scheme.size() != file.size()
        || cc::string_view::matching_prefix_of(scheme, file, cc::equal_case_insensitive{}).size() != file.size())
        return cc::nullopt;

    auto path = cc::percent_decode(parsed.value().path());
    auto host = cc::percent_decode(parsed.value().host());
    if (!path.has_value() || !host.has_value())
        return cc::nullopt;

    // `localhost` names this machine (RFC 8089), so `file://localhost/x` is the local `/x`
    constexpr auto localhost = cc::string_view("localhost");
    auto const is_local
        = host.value().size() == localhost.size()
       && cc::string_view::matching_prefix_of(host.value(), localhost, cc::equal_case_insensitive{}).size()
              == localhost.size();
    if (!host.value().empty() && !is_local)
    {
        // `file://server/share/x` is a UNC path, as VS Code reads it
#ifdef CC_OS_WINDOWS
        auto unc = cc::string("\\\\") + host.value() + path.value();
        unc.replace_all('/', '\\');
        return unc;
#else
        return cc::string("//") + host.value() + path.value();
#endif
    }

#ifdef CC_OS_WINDOWS
    // `/c:/x` is the drive path `c:/x`, when the first segment is exactly a letter and a colon
    auto const& p = path.value();
    if (p.size() >= 3 && p[0] == '/' && (cc::is_lower(p[1]) || cc::is_upper(p[1])) && p[2] == ':'
        && (p.size() == 3 || p[3] == '/'))
        return p.substring(1);
#endif
    return path;
}

cc::string lsp::uri_of_path(cc::string_view path)
{
    auto out = cc::string("file://");
    if (path.size() >= 2 && path[1] == ':')
        out.push_back('/');
    for (auto i = isize(0); i < path.size(); ++i)
    {
        auto c = path[i];
        if (c == '\\')
            c = '/';
        if (i == 0 && path.size() >= 2 && path[1] == ':' && c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        auto const is_plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/'
                           || c == '-' || c == '_' || c == '.' || c == '~';
        if (is_plain)
            out.push_back(c);
        else
            out.appendf("%{:02X}", u32(static_cast<unsigned char>(c)));
    }
    return out;
}
