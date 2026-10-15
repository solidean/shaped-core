#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh>  // cc::move
#include <clean-core/error/exception.hh> // std::uncaught_exceptions
#include <shaped-graphics/barrier/array_declarations.hh>

using namespace cc::primitive_defines;

void sg::impl::array_declaration_book::declare_buffers(u32 id,
                                                       cc::string_view name,
                                                       cc::span<array_buffer_access const> elements)
{
    auto d = declaration{.id = id,
                         .name_index = 0,
                         .buffers = {},
                         .textures = {},
                         .is_texture = false,
                         .met = false,
                         .saw_family = false,
                         .exceptions_in_flight = std::uncaught_exceptions()};
    d.buffers.push_back_range(elements);
    add(cc::move(d), name, false);
}

void sg::impl::array_declaration_book::declare_textures(u32 id,
                                                        cc::string_view name,
                                                        cc::span<array_texture_access const> elements)
{
    auto d = declaration{.id = id,
                         .name_index = 0,
                         .buffers = {},
                         .textures = {},
                         .is_texture = true,
                         .met = false,
                         .saw_family = false,
                         .exceptions_in_flight = std::uncaught_exceptions()};
    d.textures.push_back_range(elements);
    add(cc::move(d), name, true);
}

void sg::impl::array_declaration_book::add(declaration d, cc::string_view name, bool is_texture)
{
    auto const name_index = find_or_add(name, is_texture);
    d.name_index = name_index;
    ++(is_texture ? _textures_unseen : _buffers_unseen);
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
        auto const passed = passes_met_check(_declarations[i]);
        if (!_declarations[i].saw_family)
            --(_declarations[i].is_texture ? _textures_unseen : _buffers_unseen);
        _declarations.remove_at(i);
        recount(name_index);

        // Gone before the assert, so a test that catches it finds the book consistent.
        check_met(name_index, passed);
        return true;
    }
    return false;
}

void sg::impl::array_declaration_book::end_all()
{
    auto failed = isize(-1);
    for (auto const& d : _declarations)
        if (failed < 0 && !passes_met_check(d))
            failed = d.name_index;

    _declarations.clear();
    _buffers_unseen = 0;
    _textures_unseen = 0;
    for (isize i = 0; i < _names.size(); ++i)
        if (_names[i].live > 0)
            recount(i);

    // All gone before the assert, as at withdraw.
    if (failed >= 0)
        check_met(failed, false);
}

bool sg::impl::array_declaration_book::passes_met_check(declaration const& d)
{
    // One whose bind point never bound an array of its family is dormant, as on an empty frame, and proves nothing.
    // A declaration is made after its list, so unwinding destroys it first, and an assert here would turn the
    // exception into an abort.
    return d.met || !d.saw_family || std::uncaught_exceptions() > d.exceptions_in_flight;
}

void sg::impl::array_declaration_book::check_met(isize name_index, bool passed) const
{
    CC_ASSERTF(passed,
               "an array declaration of '{}' ended without meeting its array: ops of its bind point bound groups "
               "holding {} arrays, and none of those arrays was named '{}'. Is the name misspelled?",
               _names[name_index].name, _names[name_index].is_texture ? "texture" : "buffer", _names[name_index].name);
}

void sg::impl::array_declaration_book::see_family(bool is_texture)
{
    auto& unseen = is_texture ? _textures_unseen : _buffers_unseen;
    if (unseen == 0)
        return;
    for (auto& d : _declarations)
        if (d.is_texture == is_texture)
            d.saw_family = true;
    unseen = 0;
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
    _names.push_back({.name = cc::string(name),
                      .is_texture = is_texture,
                      .generation = 0,
                      .live = 0,
                      .unmet = 0,
                      .declared = array_declarations()});
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
    n.unmet = 0;
    n.declared = array_declarations();
    for (auto const& d : _declarations)
    {
        if (d.name_index != name_index)
            continue;
        ++n.live;
        if (!d.met)
            ++n.unmet;
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

void sg::impl::array_declaration_book::meet(isize name_index)
{
    auto& n = _names[name_index];
    if (n.unmet == 0)
        return;
    for (auto& d : _declarations)
        if (d.name_index == name_index)
            d.met = true;
    n.unmet = 0;
}
