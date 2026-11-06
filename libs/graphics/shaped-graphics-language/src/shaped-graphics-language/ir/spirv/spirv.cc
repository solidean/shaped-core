#include "spirv.hh"

#include <shaped-graphics-language/ir/spirv/impl/writer.hh>

cc::vector<sgl::u32> sgl::ir::write_spirv(program const& p)
{
    return spirv::impl::writer(p).run();
}
