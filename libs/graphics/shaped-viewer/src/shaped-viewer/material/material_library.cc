#include "material_library.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/thread/mutex.hh>

namespace sv
{
material_id default_material(material_library& lib)
{
    auto const type = lib.acquire_type(builtin_material::pbr);
    CC_ASSERT(type.has_value(), "the fallback material needs the builtin 'pbr' type — register_builtin_material_types");
    return lib.acquire(material::create("default", type.value(), {}));
}

material_library material_library::create()
{
    auto lib = material_library();
    lib._state = cc::make_unique<cc::mutex<state>>();
    return lib;
}

material_type_id material_library::register_type(material_type type)
{
    auto s = _state->lock_scoped();
    if (auto const* const resident = s->type_by_hash.get_ptr(type.hash); resident != nullptr)
        return *resident;

    CC_ASSERT(!s->type_by_name.contains(type.name), "a material type name is its id — two different types may not "
                                                    "share one");

    auto const id = material_type_id(s->next_type++);
    s->type_by_hash[type.hash] = id;
    s->type_by_name[type.name] = id;
    s->types.entry(id).emplace(cc::move(type));
    return id;
}

cc::optional<material_type_id> material_library::acquire_type(cc::string_view name) const
{
    auto const s = _state->lock_scoped();
    if (auto const* const id = s->type_by_name.get_ptr(name); id != nullptr)
        return *id;
    return cc::nullopt;
}

material_type const& material_library::get_type(material_type_id id) const
{
    auto const s = _state->lock_scoped();
    auto const* const t = s->types.get_ptr(id);
    CC_ASSERT(t != nullptr, "material_library::get_type: unknown id");
    return *t;
}

bool material_library::contains_type(material_type_id id) const
{
    return _state->lock([&](state const& s) { return s.types.contains(id); });
}

material_id material_library::acquire(material m)
{
    auto s = _state->lock_scoped();

    auto const* const type = s->types.get_ptr(m.type);
    CC_ASSERT(type != nullptr, "material_library::acquire: unknown type id");
    for (auto const& o : m.overrides)
    {
        auto const* const d = type->find(o.name);
        CC_ASSERT(d != nullptr, "a material binds an attribute its type does not declare");
        CC_ASSERT(o.fits(d->format), "a material's constant is not its declaration's size");
    }

    if (auto const* const resident = s->material_by_hash.get_ptr(m.hash); resident != nullptr)
        return *resident;

    auto const id = material_id(s->next_material++);
    s->material_by_hash[m.hash] = id;
    s->material_by_name[m.name] = id;
    s->materials.entry(id).emplace(cc::move(m));
    return id;
}

cc::optional<material_id> material_library::acquire(cc::string_view name) const
{
    auto const s = _state->lock_scoped();
    if (auto const* const id = s->material_by_name.get_ptr(name); id != nullptr)
        return *id;
    return cc::nullopt;
}

material const& material_library::get(material_id id) const
{
    auto const s = _state->lock_scoped();
    auto const* const m = s->materials.get_ptr(id);
    CC_ASSERT(m != nullptr, "material_library::get: unknown id");
    return *m;
}

bool material_library::contains(material_id id) const
{
    return _state->lock([&](state const& s) { return s.materials.contains(id); });
}

isize material_library::type_count() const
{
    return _state->lock([](state const& s) { return s.types.size(); });
}

isize material_library::material_count() const
{
    return _state->lock([](state const& s) { return s.materials.size(); });
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
    // Under a lock for the same reason `acquire_shader_library` is — a memoized process-wide accessor that two threads can
    // reach has to be one, or "asked once" is only true when nobody asks twice at the same moment.
    static auto cached = cc::mutex<material_library*>(nullptr);

    return cached.lock(
        [](material_library*& lib) -> cc::result<material_library*>
        {
            if (lib != nullptr)
                return lib;

            auto r = g_acquire_material_library ? g_acquire_material_library() : impl::acquire_default_material_library();

            // A failure is deliberately not cached: it leaves a caller free to call `set_acquire_material_library` and try again.
            if (r.has_error())
                return r;
            if (r.value() == nullptr)
                return cc::error("shaped-viewer: the material library provider returned no library");

            lib = r.value();
            return lib;
        });
}
} // namespace sv
