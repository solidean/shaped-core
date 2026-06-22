#include <nexus/test.hh>

TEST("test api - basic checks")
{
    SUCCEED();
    CHECK(1 < 2);
    CHECK(1 <= 2);
    CHECK(1 + 2 == 3);
    CHECK(1 + 2 != 4);
    CHECK(1 + 3 > 2);
    CHECK(1 + 1 >= 2);

    SECTION("sec A")
    {
        CHECK(10 - 5 == 5);

        SECTION("sec A.1")
        {
            CHECK(3 != 4);
        }
    }

    SECTION("sec B")
    {
        CHECK(3 - 1 == 2);
    }
}
