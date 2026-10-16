#include <clean-core/common/utility.hh> // cc::move
#include <shaped-graphics/barrier/array_declarations.hh>

using namespace cc::primitive_defines;

void sg::impl::array_declaration_book::declare_buffers(u32 id,
                                                       cc::string_view name,
                                                       cc::span<array_buffer_access const> elements)
{
    auto d = declaration{.id = id, .name_index = 0, .buffers = {}, .textures = {}};
    d.buffers.push_back_range(elements);
    add(cc::move(d), name, false);
}

void sg::impl::array_declaration_book::declare_textures(u32 id,
                                                        cc::string_view name,
                                                        cc::span<array_texture_access const> elements)
{
    auto d = declaration{.id = id, .name_index = 0, .buffers = {}, .textures = {}};
    d.textures.push_back_range(elements);
    add(cc::move(d), name, true);
}

void sg::impl::array_declaration_book::add(declaration d, cc::string_view name, bool is_texture)
{
    auto const name_index = find_or_add(name, is_texture);
    d.name_index = name_index;
    _declarations.push_back(cc::move(d));
    recount(name_index);
}

bool sg::impl::array_declaration_book::withdraw(u32 id)
{
    for (isize i = 0; i < _declarations.size(); ++i)
    {
        if (_declarations[i].id != id)
            continue;

        auto const name_index = _declarations[i].name_index;
        _declarations.remove_at(i);
        recount(name_index);
        return true;
    }
    return false;
}

void sg::impl::array_declaration_book::end_all()
{
    _declarations.clear();
    for (isize i = 0; i < _names.size(); ++i)
        if (_names[i].live > 0)
            recount(i);
}

isize sg::impl::array_declaration_book::find(cc::string_view name, bool is_texture) const
{
    for (isize i = 0; i < _names.size(); ++i)
        if (_names[i].is_texture == is_texture && _names[i].name == name)
            return i;
    return -1;
}

isize sg::impl::array_declaration_book::find_or_add(cc::string_view name, bool is_texture)
{
    if (auto const found = find(name, is_texture); found >= 0)
        return found;
    _names.push_back(
        {.name = cc::string(name), .is_texture = is_texture, .generation = 0, .live = 0, .declared = array_declarations()});
    return _names.size() - 1;
}

sg::impl::array_declaration_book::application& sg::impl::array_declaration_book::application_for(void const* group,
                                                                                                 isize binding)
{
    return _applications[application_key{.group = group, .binding = binding}];
}

void sg::impl::array_declaration_book::recount(isize name_index)
{
    // Every declaration made or ended moves its name's generation, so no application of the old set stays current.
    auto& n = _names[name_index];
    ++n.generation;
    n.live = 0;
    n.declared = array_declarations();
    for (auto const& d : _declarations)
    {
        if (d.name_index != name_index)
            continue;
        ++n.live;
        n.declared.named = true;
        for (auto const& e : d.buffers)
        {
            n.declared.any_element = true;
            n.declared.access |= e.access;
        }
        for (auto const& e : d.textures)
        {
            n.declared.any_element = true;
            n.declared.access |= e.access;
        }
    }
}
