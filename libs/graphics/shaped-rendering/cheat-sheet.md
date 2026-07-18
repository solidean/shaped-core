# shaped-rendering cheat sheet

Render routines and helpers on top of shaped-graphics. Namespace `sr`. Depends on shaped-graphics +
shaped-shader-library. Headers are included by full path from `src/`:
`#include <shaped-rendering/<name>.hh>`.

> **Scope note:** the render-routine framework is the first feature. Concrete routines (mipmap gen,
> tonemapping, …) land later. The front door for the framework is
> [docs/render-routines.md](docs/render-routines.md); format conventions live in
> [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

```cpp
#include <shaped-rendering/all.hh>   // umbrella: routine + package + library
```

## render_routine — one unit of GPU work  (abstract base)

```cpp
#include <shaped-rendering/render_routine.hh>
class my_routine : public sr::render_routine { ... };   // subclass; override the phases you need
// protected virtuals (all default to no-ops):
void init_once(sg::context& ctx)          // first init only, NEVER on reload — persistent, shader-independent work
void init_declare(sg::context& ctx)       // first init + after every reload — acquire shaders/pipelines; NO GPU work/recording
void init_materialize(sg::command_list&)  // first init + after every reload — record GPU init work
// public entry points (usually reached through routine_handle, not called directly):
r.ensure_initialized_no_materialize(ctx)  // void — run init_once + init_declare (prewarm; before a command list exists)
r.ensure_initialized(cmd)                 // void — the above, then init_materialize. Context reached via cmd.context()
// re-init is driven by slib::current_reload_generation() (process-global); init_once state survives reloads.
```

## routine_handle<R> — cheap handle to a routine

```cpp
#include <shaped-rendering/render_routine.hh>
sr::routine_handle<R>                      // a std::shared_ptr<R> wrapper; copyable by value; created via register_routine
h.is_valid()                               // bool
h.ensure_initialized_no_materialize(ctx)   // void — forwards to the routine
h.ensure_initialized(cmd)                  // void — forwards to the routine
h.acquire(cmd)                             // -> R const&  — ensures init, then returns the routine (what a static execute() reads)
h.shared()                                 // -> std::shared_ptr<R> const&  — the routine without initializing it (escape hatch)
```

## render_routine_package — a hand-written group of routines  (abstract base)

```cpp
#include <shaped-rendering/render_routine_package.hh>
class my_package : public sr::render_routine_package { ... };  // declare routine_handle members; populate in setup()
// customary singleton convention:
//   static std::shared_ptr<my_package> acquire() { static auto i = sr::make_package<my_package>(); return i; }
void setup() override                       // protected pure virtual — acquire deps + register routines; run once by make_package
p.dependencies()                           // -> cc::span<std::shared_ptr<render_routine_package> const>  — direct deps (for the closure walk)
p.routines()                               // -> cc::span<std::shared_ptr<render_routine> const>  — this package's routines
// protected helpers (call from setup()):
depend(std::shared_ptr<render_routine_package> dep)   // void — record a dependency (typically another package's acquire())
register_routine<R>(args...)               // -> routine_handle<R>  — create a routine owned by this package; return a handle to store as a member

sr::make_package<P>()                      // -> std::shared_ptr<P>  — construct P + run setup() once, with a dependency-cycle guard.
                                           //   The singleton acquire() wraps this; tests call it for a fresh, isolated instance.
```

## render_routine_library — the one object you keep around

```cpp
#include <shaped-rendering/render_routine_library.hh>
sr::render_routine_library lib;            // non-copyable; not a singleton (tests/subsystems may each build one)
lib.add_package(package)                    // void — add a package + its transitive dependency closure; deduped by instance identity
lib.ensure_all_initialized_no_materialize(ctx)   // void — prewarm every routine (init_once + init_declare); fans out all async compiles at once
lib.ensure_all_initialized(cmd)            // void — fully init every routine (declare + materialize); needs a command list (uses cmd.context())
lib.routines()                             // -> cc::span<std::shared_ptr<render_routine> const>  — the flat list, in closure order (diagnostics)
lib.packages()                             // -> cc::span<std::shared_ptr<render_routine_package> const>  — roots + transitive deps, deduped
// no shader-library wiring: reload tracking is global (slib::current_reload_generation), read by each routine directly.
```

## Typical wiring

```cpp
sr::render_routine_library lib;
lib.add_package(my_package::acquire());                  // singleton + its dependency closure
lib.ensure_all_initialized_no_materialize(*ctx);         // prewarm (parallel startup) before opening a list
// per frame:
lib.ensure_all_initialized(*cmd);                        // materialize what a reload invalidated
my_routine::execute(my_package::acquire()->my_routine, *cmd, target);  // run a routine (acquire + dispatch)
```
