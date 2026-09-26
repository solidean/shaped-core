#include "documents.hh"

#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>

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

namespace
{
[[nodiscard]] int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
} // namespace

cc::optional<cc::string> lsp::path_of_uri(cc::string_view uri)
{
    constexpr auto scheme = cc::string_view("file://");
    if (!uri.starts_with(scheme))
        return cc::nullopt;
    uri.remove_prefix(scheme.size());

    auto path = cc::string();
    for (auto i = isize(0); i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size() && hex_value(uri[i + 1]) >= 0 && hex_value(uri[i + 2]) >= 0)
        {
            path.push_back(char(hex_value(uri[i + 1]) * 16 + hex_value(uri[i + 2])));
            i += 2;
        }
        else
            path.push_back(uri[i]);
    }
    // `/c:/x` is the Windows path `c:/x`
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':')
        path = path.substring(1);
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
