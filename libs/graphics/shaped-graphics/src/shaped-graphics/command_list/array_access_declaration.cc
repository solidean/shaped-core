#include <shaped-graphics/command_list/array_access_declaration.hh>
#include <shaped-graphics/command_list/command_list.hh>

sg::array_access_declaration& sg::array_access_declaration::operator=(array_access_declaration&& rhs) noexcept
{
    if (this != &rhs)
    {
        withdraw();
        _token = cc::move(rhs._token);
    }
    return *this;
}

sg::array_access_declaration::~array_access_declaration()
{
    withdraw();
}

void sg::array_access_declaration::withdraw()
{
    auto const token = cc::move(_token);
    if (token != nullptr && token->list != nullptr)
        token->list->end_array_declaration(*token);
}
