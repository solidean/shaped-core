#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move, cc::forward
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/render_routine.hh> // routine_handle

#include <memory>
#include <type_traits>

namespace sr
{
/// A hand-written group of related routines — the unit you reach routines through. Unlike a shader
/// package (a generated static description), a routine package is a live C++ object: subclass it,
/// declare routine_handle members, and populate them in setup().
///
/// A package does not know any library. It is customary (not required) to make one a process-wide
/// singleton via a static acquire() that returns a shared_ptr and lazily builds it with make_package;
/// a library then just references the package and its transitive dependency closure.
///
///   class postprocess_package : public sr::render_routine_package
///   {
///   public:
///       static std::shared_ptr<postprocess_package> acquire()
///       {
///           static auto instance = sr::make_package<postprocess_package>();
///           return instance;
///       }
///       sr::routine_handle<vignette_routine> vignette;
///   protected:
///       void setup() override
///       {
///           _texops = texture_ops_package::acquire();
///           depend(_texops);                              // record the dependency for the closure walk
///           vignette = register_routine<vignette_routine>();
///       }
///   private:
///       std::shared_ptr<texture_ops_package> _texops;     // keep the typed handle to use its routines
///   };
///
/// See libs/graphics/shaped-rendering/docs/render-routines.md.
class render_routine_package
{
public:
    virtual ~render_routine_package() = default;

    render_routine_package(render_routine_package const&) = delete;
    render_routine_package& operator=(render_routine_package const&) = delete;

    /// This package's direct dependencies, in the order setup() declared them. A library walks these
    /// transitively to gather every routine it should drive.
    [[nodiscard]] cc::span<std::shared_ptr<render_routine_package> const> dependencies() const { return _dependencies; }

    /// The routines this package registered, in registration order.
    [[nodiscard]] cc::span<std::shared_ptr<render_routine> const> routines() const { return _routines; }

protected:
    render_routine_package() = default;

    /// Acquire dependencies and register routines here. Run once by make_package right after
    /// construction, and never again.
    virtual void setup() = 0;

    /// Record another package as a dependency (typically its acquire() result), so a library that pulls
    /// in this package also pulls in that one's routines. Keep the typed shared_ptr yourself if you need
    /// to call into the dependency.
    void depend(std::shared_ptr<render_routine_package> dependency)
    {
        CC_ASSERT(dependency != nullptr, "cannot depend on a null package");
        _dependencies.push_back(cc::move(dependency));
    }

    /// Create a routine owned by this package and return a handle to keep as a member. Args are forwarded
    /// to the routine's constructor.
    template <class R, class... Args>
    routine_handle<R> register_routine(Args&&... args)
    {
        static_assert(std::is_base_of_v<render_routine, R>, "R must derive from render_routine");
        auto routine = std::make_shared<R>(cc::forward<Args>(args)...);
        auto handle = routine_handle<R>(routine);
        _routines.push_back(cc::move(routine));
        return handle;
    }

private:
    template <class P>
    friend std::shared_ptr<P> make_package();

    cc::vector<std::shared_ptr<render_routine_package>> _dependencies;
    cc::vector<std::shared_ptr<render_routine>> _routines;
};

namespace impl
{
/// Unique, RTTI-free key per package type: each instantiation owns a distinct static address.
template <class T>
void const* package_type_key()
{
    static char const key = 0;
    return &key;
}

/// Package type keys currently under construction on this thread — the dependency-cycle guard for
/// make_package. Thread-local; defined in render_routine_package.cc.
cc::vector<void const*>& package_construction_stack();
} // namespace impl

/// Construct a package and run its setup() once, guarding against dependency cycles. This is the single
/// construct-and-initialize path: the customary singleton acquire() wraps it in a function-local static,
/// and a test that wants a fresh instance calls it directly. A cycle (A depends on B depends on A,
/// resolved through make_package) asserts rather than recursing forever.
template <class P>
[[nodiscard]] std::shared_ptr<P> make_package()
{
    static_assert(std::is_base_of_v<render_routine_package, P>, "P must derive from render_routine_package");

    void const* const key = impl::package_type_key<P>();
    auto& stack = impl::package_construction_stack();
    for (auto const* const k : stack)
        CC_ASSERT(k != key, "render routine package dependency cycle");

    stack.push_back(key);
    // RAII so the stack is restored even if setup() (or a cycle assert in a nested make_package) throws —
    // in a test, CC_ASSERT throws, so without this a caught cycle would leave the guard contaminated.
    struct scope_pop
    {
        cc::vector<void const*>& stack;
        ~scope_pop() { stack.remove_back(); }
    } const pop{stack};

    auto package = std::make_shared<P>();
    // Reach setup() through the base: this template is a friend of render_routine_package, and friendship
    // does not extend to P's protected override — a base reference makes lookup find the base's.
    static_cast<render_routine_package&>(*package).setup();

    return package;
}
} // namespace sr
