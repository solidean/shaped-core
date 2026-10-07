#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/raw_texture.hh>

namespace sg
{
command_list::~command_list()
{
    // A drop, or the list consumed: a value destroyed later must find its token dead rather than this list.
    end_array_declarations(false);
}

array_access_declaration command_list::adopt_array_declaration(u32 id, bool raster)
{
    auto link = std::make_shared<impl::array_declaration_link>(impl::array_declaration_link{
        .list = this,
        .id = id,
        .raster = raster,
    });
    _live_array_declarations.push_back(link);
    return array_access_declaration(cc::move(link));
}

void command_list::end_array_declaration(impl::array_declaration_link& link)
{
    CC_ASSERT(link.list == this, "an array declaration was ended through a list it was not made on");
    _live_array_declarations.remove_first_where([&](impl::array_declaration_token const& t) { return t.get() == &link; });
    link.list = nullptr;
    withdraw_array_declaration(link.id);
}

void command_list::end_array_declarations(bool raster_only)
{
    for (auto const& link : _live_array_declarations)
        if (!raster_only || link->raster)
            link->list = nullptr;
    _live_array_declarations.remove_all_where([](impl::array_declaration_token const& t) { return t->list == nullptr; });
}

void command_list::ensure_layout(raw_texture_handle texture, texture_layout layout, cc::optional<subresource_range> range)
{
    CC_ASSERT(texture != nullptr, "ensure_layout: texture is null");
    CC_ASSERT(layout != texture_layout::undefined, "ensure_layout: undefined is not a layout to leave a texture in");
    transition_texture_layout(cc::move(texture), layout, range);
}

void command_list::prepare_for_async(raw_texture_handle texture,
                                     async_direction direction,
                                     cc::optional<subresource_range> range)
{
    CC_ASSERT(texture != nullptr, "prepare_for_async: texture is null");
    ensure_layout(cc::move(texture), context().async_ready_layout(direction), range);
}

command_list::command_list(sg::context& ctx, epoch created_in)
  : upload(*this),
    download(*this),
    copy(*this),
    compute(*this),
    raster(*this),
    raytracing(*this),
    query(*this),
    _epoch(created_in),
    _context(&ctx)
{
    // The scopes only store the back-reference; they don't touch any not-yet-constructed member.
}

void command_list::note_render_pass_split(cc::string_view cause, split_remedy remedy)
{
    _stats.add(stat::render_pass_splits);
    if (!context().claim_render_pass_split_warning(cause))
        return;

    auto const advice = remedy == split_remedy::split_scope_between_draws
                          ? cc::string_view("split the scope between those draws")
                          : cc::string_view("record it before the scope opens");

    CC_LOG_WARNING("a rendering scope{}{} was closed and reopened around {}, which costs a store and a reload of every "
                   "target; {}, or turn this off with ctx.metrics.set_render_pass_split_warnings(false). Further "
                   "splits for this cause are counted in the render_pass_splits stat and not reported again",
                   _rendering_target_set.empty() ? "" : " of ", _rendering_target_set, cause, advice);
}
} // namespace sg

sg::impl::stat_counts const& sg::impl::recorded_stats(command_list const& cmd)
{
    return cmd._stats;
}

void sg::impl::check_no_live_array_declarations(command_list const& cmd)
{
    CC_ASSERT(cmd._live_array_declarations.empty(),
              "a command list was submitted while an array declaration made on it is alive. Withdraw it, or let it go "
              "out of scope, before the submit");
}
