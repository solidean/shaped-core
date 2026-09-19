#include "thread_scopes.hh"

#include <clean-core/record/desc.hh>
#include <clean-core/record/impl/published_blocks.hh>
#include <clean-core/record/impl/system_state.hh>
#include <clean-core/record/record.hh>
#include <clean-core/record/system.hh>

#include <cstdio> // stderr, to keep the report path as small as the crash path it shares

using namespace cc::primitive_defines;

namespace
{
/// One thread's scope stack, rebuilt from its published events.
///
/// A fixed array rather than a container: this runs from a crash handler, where allocating is the failure the whole
/// path exists to survive.
struct scope_stack
{
    cc::rec::desc const* levels[cc::rec::max_reported_scope_depth] = {};

    /// The true depth, which may exceed what `levels` holds.
    u32 depth = 0;

    void clear()
    {
        for (auto& l : levels)
            l = nullptr;
        depth = 0;
    }

    void set_level(u32 level, cc::rec::desc const* d)
    {
        if (level < u32(cc::rec::max_reported_scope_depth))
            levels[level] = d;
    }

    void push(cc::rec::desc const* d)
    {
        set_level(depth, d);
        ++depth;
    }

    void pop()
    {
        if (depth == 0)
            return;
        --depth;
        set_level(depth, nullptr);
    }
};

/// Replays one block's events onto `stack`.
///
/// A preamble RESTATES the stack rather than adding to it, which is what lets a reader that joined mid-stream know
/// the depth at all — see the stream_state fields in writer.cc.
void replay(cc::rec::chunk_view const& view, scope_stack& stack)
{
    for (auto const& e : view)
    {
        switch (e.kind())
        {
        case cc::rec::event_kind::stream_state:
        {
            stack.clear();
            stack.depth = u32(e.field_as_u64("scope_depth").value_or(0));

            // The outermost few, which is all a fixed-size preamble can carry.
            // Anything between them and the innermost stays null: open, and not nameable from here.
            auto const named = u32(e.field_as_u64("named_scopes").value_or(0));
            cc::string_view const slots[] = {"scope0", "scope1", "scope2"};
            for (u32 i = 0; i < named && i < u32(CC_ARRAY_COUNT_OF(slots)); ++i)
                stack.set_level(i, e.field_as_desc(slots[i]));
            break;
        }

        case cc::rec::event_kind::scope_begin:
            stack.push(e.desc);
            break;

        case cc::rec::event_kind::scope_end:
            stack.pop();
            break;

        default:
            break;
        }
    }
}
} // namespace

bool cc::rec::try_read_thread_scopes(cc::function_ref<void(cc::rec::thread_scope_view const&)> f)
{
    if (!rec::is_initialized())
        return false;

    return rec::impl::try_for_each_thread_state(
        [&](rec::impl::thread_state& ts)
        {
            auto stack = scope_stack();
            auto name = cc::string_view();
            auto saw_block = false;

            rec::impl::for_each_published_block(ts,
                                                [&](rec::chunk_view const& view)
                                                {
                                                    // A listener's own chunks are a different stream on the same
                                                    // thread, and replaying them would interleave two stacks into
                                                    // one.
                                                    if (view.layer != rec::chunk::no_layer)
                                                        return true;

                                                    name = view.thread.name;
                                                    saw_block = true;
                                                    replay(view, stack);
                                                    return true;
                                                });

            auto const reportable = cc::min(isize(stack.depth), isize(rec::max_reported_scope_depth));

            f(rec::thread_scope_view{
                .id = ts.tid,
                .native_tid = ts.native_tid,
                .index = ts.index,
                .name = saw_block ? name : cc::string_view(ts.name),
                .is_alive = ts.is_alive.load(cc::memory_order_acquire),
                .depth = stack.depth,
                .levels = cc::span<rec::desc const* const>(stack.levels, reportable),
            });
        });
}

void cc::rec::report_thread_scopes(char const* reason) noexcept
{
    std::fputs("\nopen scopes, per thread", stderr);
    if (reason != nullptr)
    {
        std::fputs(" (", stderr);
        std::fputs(reason, stderr);
        std::fputc(')', stderr);
    }
    std::fputc('\n', stderr);

    if (!rec::is_initialized())
    {
        // Told apart from a busy registry on purpose: one is a program that records nothing, the other is a report
        // that could not be taken, and a reader chasing a deadlock needs to know which.
        std::fputs("  <no recorder in this process>\n", stderr);
        std::fflush(stderr);
        return;
    }

    auto any = false;
    auto const read = rec::try_read_thread_scopes(
        [&](rec::thread_scope_view const& t)
        {
            any = true;

            std::fputs("  thread ", stderr);
            if (!t.name.empty())
                std::fwrite(t.name.data(), 1, size_t(t.name.size()), stderr);
            else
                std::fputs("<unnamed>", stderr);

            // The OS's id too, which is what the machine stacks above it are listed under.
            if (t.native_tid != 0)
            {
                char digits[24] = {};
                auto n = 0;
                for (auto v = t.native_tid; v != 0; v /= 10)
                    digits[n++] = char('0' + v % 10);
                std::fputs(" (tid ", stderr);
                while (n > 0)
                    std::fputc(digits[--n], stderr);
                std::fputc(')', stderr);
            }
            if (!t.is_alive)
                std::fputs(" [exited]", stderr);

            if (t.depth == 0)
            {
                std::fputs(": no scope open\n", stderr);
                return;
            }

            std::fputs(":\n", stderr);
            for (auto i = isize(0); i < t.levels.size(); ++i)
            {
                std::fputs("    ", stderr);
                for (auto indent = isize(0); indent < i; ++indent)
                    std::fputs("  ", stderr);

                auto const* const d = t.levels[i];
                if (d != nullptr && d->name != nullptr)
                    std::fputs(d->name, stderr);
                else
                    std::fputs("<unnamed scope>", stderr);
                std::fputc('\n', stderr);
            }

            // Said explicitly rather than left to be inferred from a short list.
            if (isize(t.depth) > t.levels.size())
                std::fputs("    ... deeper still\n", stderr);
        });

    if (!read)
        std::fputs("  <not read; the recorder's thread registry is busy>\n", stderr);
    else if (!any)
        std::fputs("  <no recording threads>\n", stderr);

    std::fflush(stderr);
}
