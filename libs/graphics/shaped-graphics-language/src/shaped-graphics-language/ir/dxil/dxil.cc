#include "dxil.hh"

#include <shaped-graphics-language/ir/dxil/impl/writer.hh>

cc::optional<cc::string> sgl::ir::write_dxil(program const& p, cc::vector<sgl::byte>& out)
{
    auto w = dxil::impl::writer(p);
    w.run();
    if (!w.failure().empty())
        return w.failure();
    out = dxil::impl::write_container(w);
    return {};
}
