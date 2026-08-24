#include "material_library.hh"

#include <clean-core/common/assert.hh>

namespace sv
{
material_library material_library::create()
{
    return {};
}

material_type_id material_library::register_type(material_type type)
{
    if (auto const* const resident = _type_by_hash.get_ptr(type.hash); resident != nullptr)
        return *resident;

    CC_ASSERT(!_type_by_name.contains(type.name), "a material type name is its id — two different types may not share "
                                                  "one");

    auto const id = material_type_id(_next_type++);
    _type_by_hash[type.hash] = id;
    _type_by_name[type.name] = id;
    _types.entry(id).emplace(cc::move(type));
    return id;
}

cc::optional<material_type_id> material_library::acquire_type(cc::string_view name) const
{
    if (auto const* const id = _type_by_name.get_ptr(name); id != nullptr)
        return *id;
    return cc::nullopt;
}

material_type const& material_library::get_type(material_type_id id) const
{
    auto const* const t = _types.get_ptr(id);
    CC_ASSERT(t != nullptr, "material_library::get_type: unknown id");
    return *t;
}

bool material_library::contains_type(material_type_id id) const
{
    return _types.contains(id);
}

material_id material_library::acquire(material m)
{
    auto const& type = get_type(m.type);
    for (auto const& o : m.overrides)
    {
        auto const* const d = type.find(o.name);
        CC_ASSERT(d != nullptr, "a material binds an attribute its type does not declare");
        CC_ASSERT(o.fits(d->format), "a material's constant is not its declaration's size");
    }

    if (auto const* const resident = _material_by_hash.get_ptr(m.hash); resident != nullptr)
        return *resident;

    auto const id = material_id(_next_material++);
    _material_by_hash[m.hash] = id;
    _material_by_name[m.name] = id;
    _materials.entry(id).emplace(cc::move(m));
    return id;
}

cc::optional<material_id> material_library::acquire(cc::string_view name) const
{
    if (auto const* const id = _material_by_name.get_ptr(name); id != nullptr)
        return *id;
    return cc::nullopt;
}

material const& material_library::get(material_id id) const
{
    auto const* const m = _materials.get_ptr(id);
    CC_ASSERT(m != nullptr, "material_library::get: unknown id");
    return *m;
}

bool material_library::contains(material_id id) const
{
    return _materials.contains(id);
}

namespace
{
/// The caller's provider, unset until `set_acquire_material_library` is called.
/// It lives here rather than in the header so the only way to reach it is the setter — nothing can read it, and no translation
/// unit can race the others to initialize it.
material_library_provider g_acquire_material_library;
} // namespace

void set_acquire_material_library(material_library_provider provider)
{
    g_acquire_material_library = cc::move(provider);
}

cc::result<material_library*> impl::acquire_default_material_library()
{
    // Function-local so the builtins are registered exactly once, on the first call, rather than at static-init time where no
    // ordering against the rest of sv holds.
    static auto lib = material_library::create();
    static auto const registered = []
    {
        register_builtin_material_types(lib);
        return true;
    }();
    (void)registered;
    return &lib;
}

cc::result<material_library*> acquire_material_library()
{
    // One library for the process: whoever answers is asked once, and every later caller gets that same pointer.
    static material_library* cached = nullptr;
    if (cached != nullptr)
        return cached;

    auto r = g_acquire_material_library ? g_acquire_material_library() : impl::acquire_default_material_library();

    // A failure is deliberately not cached: it leaves a caller free to call `set_acquire_material_library` and try again.
    if (r.has_error())
        return r;
    if (r.value() == nullptr)
        return cc::error("shaped-viewer: the material library provider returned no library");

    cached = r.value();
    return cached;
}
} // namespace sv
