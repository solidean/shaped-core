#include <shaped-graphics/context.hh>

namespace sg
{
context::~context() = default;

context::context(backend_kind backend) : _backend(backend)
{
}
} // namespace sg
