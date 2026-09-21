#include "token.hh"

#include <clean-core/common/assert.hh>

cc::string_view sgl::to_string(token_kind kind)
{
    switch (kind)
    {
    case token_kind::symbol:
        return "symbol";
    case token_kind::wildcard:
        return "wildcard";
    case token_kind::dot:
        return "dot";
    case token_kind::comma:
        return "comma";
    case token_kind::semicolon:
        return "semicolon";
    case token_kind::colon:
        return "colon";
    case token_kind::double_colon:
        return "double_colon";
    case token_kind::arrow:
        return "arrow";
    case token_kind::double_arrow:
        return "double_arrow";
    case token_kind::op:
        return "op";
    case token_kind::round_open:
        return "round_open";
    case token_kind::round_close:
        return "round_close";
    case token_kind::square_open:
        return "square_open";
    case token_kind::square_close:
        return "square_close";
    case token_kind::curly_open:
        return "curly_open";
    case token_kind::curly_close:
        return "curly_close";
    case token_kind::quote_open:
        return "quote_open";
    case token_kind::quote_close:
        return "quote_close";
    case token_kind::string_body:
        return "string_body";
    case token_kind::dollar:
        return "dollar";
    case token_kind::comment:
        return "comment";
    case token_kind::error:
        return "error";
    }
    CC_UNREACHABLE("unknown token_kind");
}
