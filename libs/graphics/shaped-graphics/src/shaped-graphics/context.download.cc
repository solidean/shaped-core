#include <shaped-graphics/context.download.hh>
#include <shaped-graphics/context.hh>

namespace sg
{
void context_download_scope::set_budget(cc::isize bytes)
{
    _ctx.set_inline_download_budget(bytes);
}
} // namespace sg
