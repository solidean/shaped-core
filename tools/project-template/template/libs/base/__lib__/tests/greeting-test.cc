#include <__lib__/greeting.hh>
#include <nexus/test.hh>

TEST("greeting - names who it greets")
{
    CHECK(__ns__::greeting("world") == "hello, world");
}
