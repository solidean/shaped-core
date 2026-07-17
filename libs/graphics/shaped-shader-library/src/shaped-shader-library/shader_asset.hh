#pragma once

#include <clean-core/container/small_vector.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/fwd.hh>
#include <shaped-shader-library/shader_compiler.hh>

namespace slib
{
/// One shader, compiled on demand and kept current across reloads.
///
/// acquire() is the whole consumer surface, and hot reload is invisible to it apart from the shader it
/// returns changing. Call sites reach an asset through its generated package symbol:
///
///   auto cs = sg::test::shaders::invert.compute.main->acquire(ctx);
///
/// Compilation is lazy and per format: nothing is compiled until the first acquire for a given format.
///
/// Reload never blocks a consumer. The watcher only *stages* a recompile; the next acquire promotes it
/// once it is ready, and until then keeps handing back the last shader that compiled. A broken edit
/// therefore degrades to "the old shader keeps running, and last_error() says why" rather than a stall
/// or a crash.
class shader_asset
{
public:
    /// `library` is weak on purpose — see the note on the members.
    shader_asset(std::weak_ptr<shader_library> library,
                 cc::string virtual_path,
                 sg::shader_stage stage,
                 cc::string entry_point);

    /// The compiled shader in a format `ctx` accepts, preferring the context's own order. The result
    /// carries an async error if no registered compiler connects this package's language to any format
    /// the context takes.
    [[nodiscard]] sg::async_compiled_shader acquire(sg::context const& ctx) const;

    /// The compiled shader in exactly `format`. For tests and tools that have no context.
    [[nodiscard]] sg::async_compiled_shader acquire(sg::shader_format format) const;

    /// Bumped whenever a reload replaces the shader for any format. Cache it to know when to rebuild a
    /// pipeline; it only moves inside acquire(), which is where a staged compile is promoted.
    [[nodiscard]] u64 generation() const;

    /// Why the most recent reload was rejected, if it was. Cleared by the next successful promotion.
    [[nodiscard]] cc::optional<cc::string> last_error() const;

    /// The virtual path of this shader's own source (not its includes).
    [[nodiscard]] cc::string_view virtual_path() const { return _virtual_path; }
    [[nodiscard]] sg::shader_stage stage() const { return _stage; }
    [[nodiscard]] cc::string_view entry_point() const { return _entry_point; }

    /// Every virtual path this shader was built from — its source plus the includes that were resolved,
    /// across all formats compiled so far. What the reload watcher polls. Empty before the first acquire.
    [[nodiscard]] cc::vector<cc::string> dependencies() const;

    /// Stages a recompile of every format compiled so far. Called by the reload watcher; the next
    /// acquire() picks the results up. Does nothing for formats never acquired.
    void stage_reload();

private:
    /// One target format's state. `current` is the last shader that compiled; `pending` is a staged
    /// reload not yet promoted.
    struct format_entry
    {
        sg::shader_format format = sg::shader_format::dxil;
        sg::async_compiled_shader current;
        sg::async_compiled_shader pending;
        cc::vector<cc::string> dependencies; ///< source + resolved includes, as of the last compile
    };

    struct state
    {
        // Linear scan over a handful of formats — a shader is consumed by one or two backends.
        cc::small_vector<format_entry, 2> formats;
        u64 generation = 0;
        cc::optional<cc::string> last_error;
    };

    /// Promotes a ready `pending` into `current` and bumps the generation. Caller holds the lock.
    void promote_pending(shader_library& library, state& s, format_entry& entry) const;

    /// Weak, not a plain back-reference: this asset is reachable through a generated global, and those
    /// are statics that can outlive any library. Weak turns "acquire through a stale global" into a
    /// reported error instead of a dangling read. It does not keep the library alive — the library
    /// must still outlive any acquire actually in progress.
    std::weak_ptr<shader_library> _library;

    cc::string _virtual_path;
    sg::shader_stage _stage;
    cc::string _entry_point;

    // The watcher stages compiles from its own thread while a consumer acquires. Mutable so acquire()
    // stays const: promoting a staged compile is not a change a caller can observe as one.
    mutable cc::mutex<state> _state;
};
} // namespace slib
