#include "level.hh"

namespace cnet
{
cc::string_view to_string(http_level level)
{
    switch (level)
    {
    case http_level::fetch:
        return "fetch";
    case http_level::client:
        return "client";
    case http_level::connection:
        return "connection";
    }
    return "unknown";
}
} // namespace cnet
