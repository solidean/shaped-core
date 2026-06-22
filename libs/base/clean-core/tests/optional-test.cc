#include <clean-core/optional.hh>

#include <nexus/test.hh>

#include <memory>
#include <string>

// optional stays trivial
static_assert(std::is_constructible_v<cc::optional<int>>);
static_assert(std::is_constructible_v<cc::optional<int>, int>);
static_assert(std::is_constructible_v<cc::optional<int>, cc::nullopt_t>);
static_assert(std::is_trivially_copyable_v<cc::optional<int>>);
static_assert(std::is_trivially_destructible_v<cc::optional<int>>);

namespace
{
// test type for non-trivial operations
struct non_trivial
{
    int value = 0;
    bool* destroyed = nullptr;

    non_trivial() = default;
    explicit non_trivial(int v) : value(v) {}
    non_trivial(int v, bool* d) : value(v), destroyed(d) {}

    ~non_trivial()
    {
        if (destroyed)
            *destroyed = true;
    }

    non_trivial(non_trivial const&) = default;
    non_trivial(non_trivial&&) = default;
    non_trivial& operator=(non_trivial const&) = default;
    non_trivial& operator=(non_trivial&&) = default;

    friend bool operator==(non_trivial const&, non_trivial const&) = default;
};

// move-only type for testing
struct move_only
{
    int value = 0;

    move_only() = default;
    explicit move_only(int v) : value(v) {}

    move_only(move_only const&) = delete;
    move_only(move_only&& rhs) noexcept : value(rhs.value) { rhs.value = -1; }
    move_only& operator=(move_only const&) = delete;
    move_only& operator=(move_only&& rhs) noexcept
    {
        value = rhs.value;
        rhs.value = -1;
        return *this;
    }

    ~move_only() = default;
};

// counting type to track special member function calls
struct counting_type
{
    int value = 0;

    static inline int default_ctor_count = 0;
    static inline int value_ctor_count = 0;
    static inline int copy_ctor_count = 0;
    static inline int move_ctor_count = 0;
    static inline int copy_assign_count = 0;
    static inline int move_assign_count = 0;
    static inline int dtor_count = 0;

    static void reset_counters()
    {
        default_ctor_count = 0;
        value_ctor_count = 0;
        copy_ctor_count = 0;
        move_ctor_count = 0;
        copy_assign_count = 0;
        move_assign_count = 0;
        dtor_count = 0;
    }

    counting_type() { ++default_ctor_count; }
    explicit counting_type(int v) : value(v) { ++value_ctor_count; }

    counting_type(counting_type const& rhs) : value(rhs.value) { ++copy_ctor_count; }
    counting_type(counting_type&& rhs) noexcept : value(rhs.value) { ++move_ctor_count; }

    counting_type& operator=(counting_type const& rhs)
    {
        value = rhs.value;
        ++copy_assign_count;
        return *this;
    }

    counting_type& operator=(counting_type&& rhs) noexcept
    {
        value = rhs.value;
        ++move_assign_count;
        return *this;
    }

    ~counting_type() { ++dtor_count; }

    friend bool operator==(counting_type const&, counting_type const&) = default;
};
} // namespace

TEST("optional - trivial types")
{
    SECTION("default construction")
    {
        auto const opt = cc::optional<int>{};
        CHECK(!opt.has_value());
    }

    SECTION("nullopt construction")
    {
        auto const opt = cc::optional<int>{cc::nullopt};
        CHECK(!opt.has_value());
    }

    SECTION("value construction")
    {
        auto const opt = cc::optional<int>{42};
        CHECK(opt.has_value());
        CHECK(opt.value() == 42);
    }

    SECTION("copy construction")
    {
        auto const opt1 = cc::optional<int>{42};
        auto const opt2 = opt1;
        CHECK(opt2.has_value());
        CHECK(opt2.value() == 42);
        CHECK(opt1.has_value());
        CHECK(opt1.value() == 42);
    }

    SECTION("move construction")
    {
        auto opt1 = cc::optional<int>{42};
        auto const opt2 = cc::move(opt1);
        CHECK(opt2.has_value());
        CHECK(opt2.value() == 42);
        // trivial types remain engaged after move
        CHECK(opt1.has_value());
        CHECK(opt1.value() == 42);
    }

    SECTION("copy assignment")
    {
        auto opt1 = cc::optional<int>{42};
        auto opt2 = cc::optional<int>{};
        opt2 = opt1;
        CHECK(opt2.has_value());
        CHECK(opt2.value() == 42);
        CHECK(opt1.has_value());
        CHECK(opt1.value() == 42);
    }

    SECTION("move assignment")
    {
        auto opt1 = cc::optional<int>{42};
        auto opt2 = cc::optional<int>{};
        opt2 = cc::move(opt1);
        CHECK(opt2.has_value());
        CHECK(opt2.value() == 42);
        // trivial types remain engaged after move
        CHECK(opt1.has_value());
        CHECK(opt1.value() == 42);
    }

    SECTION("value assignment")
    {
        auto opt = cc::optional<int>{};
        opt = 42;
        CHECK(opt.has_value());
        CHECK(opt.value() == 42);
    }

    SECTION("nullopt assignment")
    {
        auto opt = cc::optional<int>{42};
        opt = cc::nullopt;
        CHECK(!opt.has_value());
    }
}

TEST("optional - non-trivial types")
{
    SECTION("default construction")
    {
        auto const opt = cc::optional<non_trivial>{};
        CHECK(!opt.has_value());
    }

    SECTION("value construction")
    {
        auto const opt = cc::optional<non_trivial>{non_trivial{42}};
        CHECK(opt.has_value());
        CHECK(opt.value().value == 42);
    }

    SECTION("copy construction")
    {
        auto const opt1 = cc::optional<non_trivial>{non_trivial{42}};
        auto const opt2 = opt1; // NOLINT
        CHECK(opt2.has_value());
        CHECK(opt2.value().value == 42);
        CHECK(opt1.has_value());
        CHECK(opt1.value().value == 42);
    }

    SECTION("move construction")
    {
        auto opt1 = cc::optional<non_trivial>{non_trivial{42}};
        auto const opt2 = cc::move(opt1);
        CHECK(opt2.has_value());
        CHECK(opt2.value().value == 42);
        CHECK(!opt1.has_value());
    }

    SECTION("copy assignment")
    {
        auto opt1 = cc::optional<non_trivial>{non_trivial{42}};
        auto opt2 = cc::optional<non_trivial>{};
        opt2 = opt1;
        CHECK(opt2.has_value());
        CHECK(opt2.value().value == 42);
        CHECK(opt1.has_value());
        CHECK(opt1.value().value == 42);
    }

    SECTION("move assignment")
    {
        auto opt1 = cc::optional<non_trivial>{non_trivial{42}};
        auto opt2 = cc::optional<non_trivial>{};
        opt2 = cc::move(opt1);
        CHECK(opt2.has_value());
        CHECK(opt2.value().value == 42);
        CHECK(opt1.has_value()); // still has its value, just in moved-from form
    }

    SECTION("value types - string")
    {
        auto opt = cc::optional<std::string>{"hello"};
        CHECK(opt.has_value());
        CHECK(opt.value() == "hello");

        opt = std::string{"world"};
        CHECK(opt.has_value());
        CHECK(opt.value() == "world");

        opt = cc::nullopt;
        CHECK(!opt.has_value());
    }
}

TEST("optional - counting special member functions")
{
    SECTION("default construction")
    {
        counting_type::reset_counters();
        {
            auto const opt = cc::optional<counting_type>{};
            CHECK(!opt.has_value());
        }
        // No construction or destruction should occur
        CHECK(counting_type::default_ctor_count == 0);
        CHECK(counting_type::value_ctor_count == 0);
        CHECK(counting_type::copy_ctor_count == 0);
        CHECK(counting_type::move_ctor_count == 0);
        CHECK(counting_type::copy_assign_count == 0);
        CHECK(counting_type::move_assign_count == 0);
        CHECK(counting_type::dtor_count == 0);
    }

    SECTION("value construction")
    {
        counting_type::reset_counters();
        {
            auto const opt = cc::optional<counting_type>{counting_type{42}};
            CHECK(opt.has_value());
            CHECK(opt.value().value == 42);
        }
        // Should construct temp value (value_ctor), move it into optional (move_ctor), then destruct both
        CHECK(counting_type::value_ctor_count == 1);
        CHECK(counting_type::move_ctor_count == 1);
        CHECK(counting_type::dtor_count == 2); // temp + optional contents
        CHECK(counting_type::copy_ctor_count == 0);
        CHECK(counting_type::copy_assign_count == 0);
        CHECK(counting_type::move_assign_count == 0);
    }

    SECTION("copy construction")
    {
        counting_type::reset_counters();
        {
            auto const opt1 = cc::optional<counting_type>{counting_type{42}};
            counting_type::reset_counters(); // Reset to isolate copy construction
            auto const opt2 = opt1;
            CHECK(opt2.has_value());
            CHECK(opt2.value().value == 42);
        }
        // Should only copy construct
        CHECK(counting_type::copy_ctor_count == 1);
        CHECK(counting_type::move_ctor_count == 0);
        CHECK(counting_type::copy_assign_count == 0);
        CHECK(counting_type::move_assign_count == 0);
        CHECK(counting_type::dtor_count == 2); // both optionals destructed
    }

    SECTION("move construction")
    {
        counting_type::reset_counters();
        {
            auto opt1 = cc::optional<counting_type>{counting_type{42}};
            counting_type::reset_counters(); // Reset to isolate move construction
            auto const opt2 = cc::move(opt1);
            CHECK(opt2.has_value());
            CHECK(opt2.value().value == 42);
            CHECK(!opt1.has_value());
        }
        // Should only move construct, then destruct the moved value
        CHECK(counting_type::move_ctor_count == 1);
        CHECK(counting_type::dtor_count == 2); // moved-from value + opt2 contents
        CHECK(counting_type::copy_ctor_count == 0);
        CHECK(counting_type::copy_assign_count == 0);
        CHECK(counting_type::move_assign_count == 0);
    }

    SECTION("copy assignment - empty to empty")
    {
        counting_type::reset_counters();
        {
            auto opt1 = cc::optional<counting_type>{};
            auto opt2 = cc::optional<counting_type>{};
            opt2 = opt1;
            CHECK(!opt2.has_value());
        }
        // No operations should occur
        CHECK(counting_type::copy_ctor_count == 0);
        CHECK(counting_type::copy_assign_count == 0);
        CHECK(counting_type::move_ctor_count == 0);
        CHECK(counting_type::move_assign_count == 0);
        CHECK(counting_type::dtor_count == 0);
    }

    SECTION("copy assignment - value to empty")
    {
        counting_type::reset_counters();
        {
            auto opt1 = cc::optional<counting_type>{counting_type{42}};
            auto opt2 = cc::optional<counting_type>{};
            counting_type::reset_counters(); // Reset to isolate assignment
            opt2 = opt1;
            CHECK(opt2.has_value());
            CHECK(opt2.value().value == 42);
        }
        // Should copy construct into empty optional
        CHECK(counting_type::copy_ctor_count == 1);
        CHECK(counting_type::copy_assign_count == 0);
        CHECK(counting_type::dtor_count == 2); // both destructed at end
    }

    SECTION("copy assignment - value to value")
    {
        counting_type::reset_counters();
        {
            auto opt1 = cc::optional<counting_type>{counting_type{42}};
            auto opt2 = cc::optional<counting_type>{counting_type{99}};
            counting_type::reset_counters(); // Reset to isolate assignment
            opt2 = opt1;
            CHECK(opt2.has_value());
            CHECK(opt2.value().value == 42);
        }
        // Should use copy assignment operator (not reconstruct)
        CHECK(counting_type::copy_assign_count == 1);
        CHECK(counting_type::copy_ctor_count == 0);
        CHECK(counting_type::dtor_count == 2); // both destructed at end
    }

    SECTION("copy assignment - empty to value")
    {
        counting_type::reset_counters();
        {
            auto opt1 = cc::optional<counting_type>{};
            auto opt2 = cc::optional<counting_type>{counting_type{99}};
            counting_type::reset_counters(); // Reset to isolate assignment
            opt2 = opt1;
            CHECK(!opt2.has_value());
        }
        // Should destruct the value in opt2
        CHECK(counting_type::dtor_count == 1);
        CHECK(counting_type::copy_ctor_count == 0);
        CHECK(counting_type::copy_assign_count == 0);
    }

    SECTION("move assignment - empty to empty")
    {
        counting_type::reset_counters();
        {
            auto opt1 = cc::optional<counting_type>{};
            auto opt2 = cc::optional<counting_type>{};
            opt2 = cc::move(opt1);
            CHECK(!opt2.has_value());
        }
        // No operations should occur
        CHECK(counting_type::move_ctor_count == 0);
        CHECK(counting_type::move_assign_count == 0);
        CHECK(counting_type::dtor_count == 0);
    }

    SECTION("move assignment - value to empty")
    {
        counting_type::reset_counters();
        {
            auto opt1 = cc::optional<counting_type>{counting_type{42}};
            auto opt2 = cc::optional<counting_type>{};
            counting_type::reset_counters(); // Reset to isolate assignment
            opt2 = cc::move(opt1);
            CHECK(opt2.has_value());
            CHECK(opt2.value().value == 42);
            CHECK(opt1.has_value()); // still has its value, just in moved-from form
        }
        // Should move construct into empty optional, then destruct moved-from value
        CHECK(counting_type::move_ctor_count == 1);
        CHECK(counting_type::move_assign_count == 0);
        CHECK(counting_type::dtor_count == 2); // moved-from + opt2 contents
    }

    SECTION("move assignment - value to value")
    {
        counting_type::reset_counters();
        {
            auto opt1 = cc::optional<counting_type>{counting_type{42}};
            auto opt2 = cc::optional<counting_type>{counting_type{99}};
            counting_type::reset_counters(); // Reset to isolate assignment
            opt2 = cc::move(opt1);
            CHECK(opt2.has_value());
            CHECK(opt2.value().value == 42);
            CHECK(opt1.has_value()); // still has its value, just in moved-from form
        }
        // Should use move assignment operator (not reconstruct)
        CHECK(counting_type::move_assign_count == 1);
        CHECK(counting_type::move_ctor_count == 0);
        CHECK(counting_type::dtor_count == 2); // moved-from + opt2 contents
    }

    SECTION("move assignment - empty to value")
    {
        counting_type::reset_counters();
        {
            auto opt1 = cc::optional<counting_type>{};
            auto opt2 = cc::optional<counting_type>{counting_type{99}};
            counting_type::reset_counters(); // Reset to isolate assignment
            opt2 = cc::move(opt1);
            CHECK(!opt2.has_value());
        }
        // Should destruct the value in opt2
        CHECK(counting_type::dtor_count == 1);
        CHECK(counting_type::move_ctor_count == 0);
        CHECK(counting_type::move_assign_count == 0);
    }

    SECTION("constructor and destructor balance")
    {
        counting_type::reset_counters();
        {
            auto opt1 = cc::optional<counting_type>{counting_type{1}};
            auto opt2 = cc::optional<counting_type>{counting_type{2}};
            auto opt3 = opt1;
            auto opt4 = cc::move(opt2);
            opt1 = opt3;
            opt2 = cc::move(opt4);
        }
        // Total constructions must equal total destructions
        int const total_ctors = counting_type::default_ctor_count + counting_type::value_ctor_count
                              + counting_type::copy_ctor_count + counting_type::move_ctor_count;
        CHECK(total_ctors == counting_type::dtor_count);
    }

    SECTION("self-copy assignment")
    {
        counting_type::reset_counters();
        {
            auto opt = cc::optional<counting_type>{counting_type{42}};
            counting_type::reset_counters(); // Reset to isolate self-assignment
            opt = opt;                       // NOLINT(clang-diagnostic-self-assign-overloaded)
            CHECK(opt.has_value());
            CHECK(opt.value().value == 42);
        }
        // Self-assignment should be a no-op (no copy, no construction)
        CHECK(counting_type::copy_assign_count == 0);
        CHECK(counting_type::copy_ctor_count == 0);
        CHECK(counting_type::move_assign_count == 0);
        CHECK(counting_type::move_ctor_count == 0);
        // Only destructor at end of scope
        CHECK(counting_type::dtor_count == 1);
    }

    SECTION("self-move assignment")
    {
        counting_type::reset_counters();
        {
            auto opt = cc::optional<counting_type>{counting_type{42}};
            counting_type::reset_counters(); // Reset to isolate self-assignment
            opt = cc::move(opt);
            // After self-move, optional is not "empty"
            CHECK(opt.has_value()); // still has its value, just in moved-from form
        }
        // Self-move should destruct the value and leave optional empty
        CHECK(counting_type::dtor_count == 1);
        CHECK(counting_type::move_assign_count == 1); // is also a self-assign
        CHECK(counting_type::move_ctor_count == 0);
        CHECK(counting_type::copy_assign_count == 0);
        CHECK(counting_type::copy_ctor_count == 0);
    }

    SECTION("self-copy assignment - empty")
    {
        counting_type::reset_counters();
        {
            auto opt = cc::optional<counting_type>{};
            opt = opt; // NOLINT(clang-diagnostic-self-assign-overloaded)
            CHECK(!opt.has_value());
        }
        // No operations should occur
        CHECK(counting_type::copy_assign_count == 0);
        CHECK(counting_type::copy_ctor_count == 0);
        CHECK(counting_type::dtor_count == 0);
    }

    SECTION("self-move assignment - empty")
    {
        counting_type::reset_counters();
        {
            auto opt = cc::optional<counting_type>{};
            opt = cc::move(opt);
            CHECK(!opt.has_value());
        }
        // No operations should occur
        CHECK(counting_type::move_assign_count == 0);
        CHECK(counting_type::move_ctor_count == 0);
        CHECK(counting_type::dtor_count == 0);
    }
}

TEST("optional - move-only types")
{
    SECTION("construction")
    {
        auto opt = cc::optional<move_only>{move_only{42}};
        CHECK(opt.has_value());
        CHECK(opt.value().value == 42);
    }

    SECTION("move construction")
    {
        auto opt1 = cc::optional<move_only>{move_only{42}};
        auto opt2 = cc::move(opt1);
        CHECK(opt2.has_value());
        CHECK(opt2.value().value == 42);
        CHECK(!opt1.has_value());
    }

    SECTION("move assignment")
    {
        auto opt1 = cc::optional<move_only>{move_only{42}};
        auto opt2 = cc::optional<move_only>{};
        opt2 = cc::move(opt1);
        CHECK(opt2.has_value());
        CHECK(opt2.value().value == 42);
        CHECK(opt1.has_value()); // still has its value, just in moved-from form
    }

    SECTION("value move assignment")
    {
        auto opt = cc::optional<move_only>{};
        opt = move_only{99};
        CHECK(opt.has_value());
        CHECK(opt.value().value == 99);
    }

    SECTION("unique_ptr")
    {
        auto opt = cc::optional<std::unique_ptr<int>>{std::make_unique<int>(42)};
        CHECK(opt.has_value());
        CHECK(*opt.value() == 42);

        auto opt2 = cc::move(opt);
        CHECK(opt2.has_value());
        CHECK(*opt2.value() == 42);
        CHECK(!opt.has_value());
    }
}

TEST("optional - equality operator")
{
    SECTION("both empty")
    {
        auto const opt1 = cc::optional<int>{};
        auto const opt2 = cc::optional<int>{};
        CHECK(opt1 == opt2);
    }

    SECTION("one empty, one with value")
    {
        auto const opt1 = cc::optional<int>{};
        auto const opt2 = cc::optional<int>{42};
        CHECK(opt1 != opt2);
        CHECK(opt2 != opt1);
    }

    SECTION("both with same value")
    {
        auto const opt1 = cc::optional<int>{42};
        auto const opt2 = cc::optional<int>{42};
        CHECK(opt1 == opt2);
    }

    SECTION("both with different values")
    {
        auto const opt1 = cc::optional<int>{42};
        auto const opt2 = cc::optional<int>{99};
        CHECK(opt1 != opt2);
    }

    SECTION("non-trivial types")
    {
        auto const opt1 = cc::optional<non_trivial>{non_trivial{42}};
        auto const opt2 = cc::optional<non_trivial>{non_trivial{42}};
        auto const opt3 = cc::optional<non_trivial>{non_trivial{99}};
        auto const opt4 = cc::optional<non_trivial>{};

        CHECK(opt1 == opt2);
        CHECK(opt1 != opt3);
        CHECK(opt1 != opt4);
        CHECK(opt4 != opt1);
    }
}

TEST("optional - value and has_value")
{
    SECTION("has_value on empty")
    {
        auto const opt = cc::optional<int>{};
        CHECK(!opt.has_value());
    }

    SECTION("has_value on filled")
    {
        auto const opt = cc::optional<int>{42};
        CHECK(opt.has_value());
    }

    SECTION("value const reference")
    {
        auto const opt = cc::optional<int>{42};
        CHECK(opt.value() == 42);
    }

    SECTION("value reference")
    {
        auto opt = cc::optional<int>{42};
        opt.value() = 99;
        CHECK(opt.value() == 99);
    }

    SECTION("value rvalue reference")
    {
        auto opt = cc::optional<move_only>{move_only{42}};
        auto moved = cc::move(opt).value();
        CHECK(moved.value == 42);
    }

    SECTION("value preserves category - const")
    {
        auto const opt = cc::optional<std::string>{"hello"};
        auto const& ref = opt.value();
        CHECK(ref == "hello");
    }

    SECTION("value preserves category - mutable")
    {
        auto opt = cc::optional<std::string>{"hello"};
        opt.value() += " world";
        CHECK(opt.value() == "hello world");
    }
}

TEST("optional - assignment scenarios")
{
    SECTION("empty to value")
    {
        auto opt = cc::optional<int>{};
        opt = 42;
        CHECK(opt.has_value());
        CHECK(opt.value() == 42);
    }

    SECTION("value to different value")
    {
        auto opt = cc::optional<int>{42};
        opt = 99;
        CHECK(opt.has_value());
        CHECK(opt.value() == 99);
    }

    SECTION("value to empty")
    {
        auto opt = cc::optional<int>{42};
        opt = cc::nullopt;
        CHECK(!opt.has_value());
    }

    SECTION("empty to empty")
    {
        auto opt = cc::optional<int>{};
        opt = cc::nullopt;
        CHECK(!opt.has_value());
    }

    SECTION("copy from empty to empty")
    {
        auto opt1 = cc::optional<int>{};
        auto opt2 = cc::optional<int>{};
        opt2 = opt1;
        CHECK(!opt2.has_value());
    }

    SECTION("copy from value to value")
    {
        auto opt1 = cc::optional<int>{42};
        auto opt2 = cc::optional<int>{99};
        opt2 = opt1;
        CHECK(opt2.has_value());
        CHECK(opt2.value() == 42);
    }

    SECTION("copy from empty to value")
    {
        auto opt1 = cc::optional<int>{};
        auto opt2 = cc::optional<int>{99};
        opt2 = opt1;
        CHECK(!opt2.has_value());
    }

    SECTION("copy from value to empty")
    {
        auto opt1 = cc::optional<int>{42};
        auto opt2 = cc::optional<int>{};
        opt2 = opt1;
        CHECK(opt2.has_value());
        CHECK(opt2.value() == 42);
    }
}

TEST("optional - subobject-safe move assignment")
{
    // Test that move assignment is subobject-safe, meaning it remains well-defined
    // even when the right-hand side aliases a subobject transitively owned by the
    // left-hand side. In other words, x = std::move(y) must not assume that y is
    // independent of x; y may live inside x.
    //
    // This test uses a recursive structure where a type contains a unique_ptr to
    // an optional of itself, creating a situation where we can move-assign from
    // a nested subobject.

    struct self_ref_foo
    {
        int value = 0;
        std::unique_ptr<cc::optional<self_ref_foo>> inner;

        self_ref_foo() = default;
        ~self_ref_foo() = default;
        explicit self_ref_foo(int v) : value(v) {}
        self_ref_foo(int v, std::unique_ptr<cc::optional<self_ref_foo>> i) : value(v), inner(cc::move(i)) {}

        self_ref_foo(self_ref_foo const&) = delete;
        self_ref_foo(self_ref_foo&& rhs) noexcept : value(rhs.value), inner(cc::move(rhs.inner)) { rhs.value = -1; }
        self_ref_foo& operator=(self_ref_foo const&) = delete;
        self_ref_foo& operator=(self_ref_foo&& rhs) noexcept
        {
            value = rhs.value;
            inner = cc::move(rhs.inner);
            // careful! rhs is dead here
            return *this;
        }
    };

    SECTION("move assignment from owned subobject")
    {
        // Create an optional containing a self_ref_foo that itself contains
        // an optional<self_ref_foo> as a nested subobject
        auto my_opt = cc::optional<self_ref_foo>{
            self_ref_foo{42, std::make_unique<cc::optional<self_ref_foo>>(self_ref_foo{99})}};

        CHECK(my_opt.has_value());
        CHECK(my_opt.value().value == 42);
        CHECK(my_opt.value().inner != nullptr);
        CHECK(my_opt.value().inner->has_value());
        CHECK(my_opt.value().inner->value().value == 99);

        // This is the critical test: move-assign from a subobject that is transitively owned
        // by the destination. The move assignment must not eagerly destroy or reset the
        // left-hand side before completing the move, as that would destroy the source.
        my_opt = cc::move(*my_opt.value().inner);

        // After the move, my_opt should contain the inner value (99)
        CHECK(my_opt.has_value());
        CHECK(my_opt.value().value == 99);
    }

    SECTION("move assignment from deeply nested subobject")
    {
        // Create a three-level nested structure
        auto inner_inner = std::make_unique<cc::optional<self_ref_foo>>(self_ref_foo{123});
        auto inner = std::make_unique<cc::optional<self_ref_foo>>(self_ref_foo{456, cc::move(inner_inner)});
        auto my_opt = cc::optional<self_ref_foo>{self_ref_foo{789, cc::move(inner)}};

        CHECK(my_opt.has_value());
        CHECK(my_opt.value().value == 789);
        CHECK(my_opt.value().inner != nullptr);
        CHECK(my_opt.value().inner->has_value());
        CHECK(my_opt.value().inner->value().value == 456);
        CHECK(my_opt.value().inner->value().inner != nullptr);
        CHECK(my_opt.value().inner->value().inner->has_value());
        CHECK(my_opt.value().inner->value().inner->value().value == 123);

        // Move from the deeply nested inner-inner optional
        my_opt = cc::move(*my_opt.value().inner->value().inner);

        CHECK(my_opt.has_value());
        CHECK(my_opt.value().value == 123);
    }
}

TEST("optional - as_span")
{
    SECTION("empty optional")
    {
        auto const opt = cc::optional<int>{};
        auto const s = opt.as_span();
        CHECK(s.size() == 0);
        CHECK(s.empty());
    }

    SECTION("optional with value")
    {
        auto opt = cc::optional<int>{42};
        auto s = opt.as_span();
        CHECK(s.size() == 1);
        CHECK(!s.empty());
        CHECK(s[0] == 42);
    }

    SECTION("const optional with value")
    {
        auto const opt = cc::optional<int>{42};
        auto const s = opt.as_span();
        CHECK(s.size() == 1);
        CHECK(!s.empty());
        CHECK(s[0] == 42);
        static_assert(std::is_same_v<decltype(s), cc::span<int const> const>);
    }

    SECTION("mutable span allows modification")
    {
        auto opt = cc::optional<int>{42};
        auto s = opt.as_span();
        s[0] = 99;
        CHECK(opt.value() == 99);
    }

    SECTION("non-trivial types")
    {
        auto opt = cc::optional<std::string>{"hello"};
        auto s = opt.as_span();
        CHECK(s.size() == 1);
        CHECK(s[0] == "hello");
        s[0] = "world";
        CHECK(opt.value() == "world");
    }

    SECTION("empty non-trivial optional")
    {
        auto const opt = cc::optional<std::string>{};
        auto const s = opt.as_span();
        CHECK(s.size() == 0);
        CHECK(s.empty());
    }

    SECTION("span can be used in range-for")
    {
        auto opt = cc::optional<int>{42};
        int count = 0;
        for (auto const& val : opt.as_span())
        {
            CHECK(val == 42);
            ++count;
        }
        CHECK(count == 1);
    }

    SECTION("empty span in range-for")
    {
        auto const opt = cc::optional<int>{};
        int count = 0;
        for (auto const& val : opt.as_span())
        {
            (void)val;
            ++count;
        }
        CHECK(count == 0);
    }
}

TEST("optional - value_or")
{
    SECTION("const lvalue - has value")
    {
        auto const opt = cc::optional<int>{42};
        CHECK(opt.value_or(99) == 42);
    }

    SECTION("const lvalue - empty")
    {
        auto const opt = cc::optional<int>{};
        CHECK(opt.value_or(99) == 99);
    }

    SECTION("rvalue - has value")
    {
        auto opt = cc::optional<int>{42};
        CHECK(cc::move(opt).value_or(99) == 42);
    }

    SECTION("rvalue - empty")
    {
        auto opt = cc::optional<int>{};
        CHECK(cc::move(opt).value_or(99) == 99);
    }

    SECTION("non-trivial type - const lvalue")
    {
        auto const opt = cc::optional<std::string>{"hello"};
        CHECK(opt.value_or("default") == "hello");
    }

    SECTION("non-trivial type - const lvalue empty")
    {
        auto const opt = cc::optional<std::string>{};
        CHECK(opt.value_or("default") == "default");
    }

    SECTION("non-trivial type - rvalue")
    {
        auto opt = cc::optional<std::string>{"hello"};
        auto result = cc::move(opt).value_or("default");
        CHECK(result == "hello");
    }

    SECTION("non-trivial type - rvalue empty")
    {
        auto opt = cc::optional<std::string>{};
        auto result = cc::move(opt).value_or("default");
        CHECK(result == "default");
    }

    SECTION("move-only type - rvalue")
    {
        auto opt = cc::optional<move_only>{move_only{42}};
        auto result = cc::move(opt).value_or(move_only{99});
        CHECK(result.value == 42);
    }

    SECTION("move-only type - rvalue empty")
    {
        auto opt = cc::optional<move_only>{};
        auto result = cc::move(opt).value_or(move_only{99});
        CHECK(result.value == 99);
    }

    SECTION("unique_ptr - has value")
    {
        auto opt = cc::optional<std::unique_ptr<int>>{std::make_unique<int>(42)};
        auto result = cc::move(opt).value_or(std::make_unique<int>(99));
        CHECK(*result == 42);
    }

    SECTION("unique_ptr - empty")
    {
        auto opt = cc::optional<std::unique_ptr<int>>{};
        auto result = cc::move(opt).value_or(std::make_unique<int>(99));
        CHECK(*result == 99);
    }

    SECTION("type conversion")
    {
        auto const opt = cc::optional<int>{42};
        CHECK(opt.value_or(99.5) == 42);
    }

    SECTION("type conversion - empty")
    {
        auto const opt = cc::optional<int>{};
        CHECK(opt.value_or(99.5) == 99);
    }
}

TEST("optional - emplace_value")
{
    SECTION("emplace into empty")
    {
        auto opt = cc::optional<int>{};
        auto& ref = opt.emplace_value(42);
        CHECK(opt.has_value());
        CHECK(opt.value() == 42);
        CHECK(&ref == &opt.value());
    }

    SECTION("emplace into existing")
    {
        auto opt = cc::optional<int>{99};
        auto& ref = opt.emplace_value(42);
        CHECK(opt.has_value());
        CHECK(opt.value() == 42);
        CHECK(&ref == &opt.value());
    }

    SECTION("emplace non-trivial type")
    {
        auto opt = cc::optional<std::string>{};
        auto& ref = opt.emplace_value("hello");
        CHECK(opt.has_value());
        CHECK(opt.value() == "hello");
        CHECK(&ref == &opt.value());
    }

    SECTION("emplace replaces existing non-trivial")
    {
        auto opt = cc::optional<std::string>{"old"};
        auto& ref = opt.emplace_value("new");
        CHECK(opt.has_value());
        CHECK(opt.value() == "new");
        CHECK(&ref == &opt.value());
    }

    SECTION("emplace with multiple arguments")
    {
        auto opt = cc::optional<std::string>{};
        auto& ref = opt.emplace_value(5, 'x');
        CHECK(opt.has_value());
        CHECK(opt.value() == "xxxxx");
        CHECK(&ref == &opt.value());
    }

    SECTION("emplace counting type - into empty")
    {
        counting_type::reset_counters();
        {
            auto opt = cc::optional<counting_type>{};
            opt.emplace_value(42);
            CHECK(opt.has_value());
            CHECK(opt.value().value == 42);
        }
        // Should construct once (value_ctor), destruct once
        CHECK(counting_type::value_ctor_count == 1);
        CHECK(counting_type::dtor_count == 1);
        CHECK(counting_type::copy_ctor_count == 0);
        CHECK(counting_type::move_ctor_count == 0);
    }

    SECTION("emplace counting type - into existing")
    {
        counting_type::reset_counters();
        {
            auto opt = cc::optional<counting_type>{counting_type{99}};
            counting_type::reset_counters(); // Reset to isolate emplace
            opt.emplace_value(42);
            CHECK(opt.has_value());
            CHECK(opt.value().value == 42);
        }
        // Should destruct old value, construct new value (value_ctor), destruct new value
        CHECK(counting_type::value_ctor_count == 1);
        CHECK(counting_type::dtor_count == 2); // old value + final destruction
        CHECK(counting_type::copy_ctor_count == 0);
        CHECK(counting_type::move_ctor_count == 0);
    }

    SECTION("emplace destructor called")
    {
        bool destroyed = false;
        {
            auto opt = cc::optional<non_trivial>::create_emplaced(99, &destroyed);
            CHECK(!destroyed);
            opt.emplace_value(42);
            CHECK(destroyed); // old value was destroyed
            CHECK(opt.value().value == 42);
        }
    }

    SECTION("emplace move-only type")
    {
        auto opt = cc::optional<move_only>{};
        opt.emplace_value(42);
        CHECK(opt.has_value());
        CHECK(opt.value().value == 42);
    }

    SECTION("emplace returns modifiable reference")
    {
        auto opt = cc::optional<int>{};
        opt.emplace_value(42) = 99;
        CHECK(opt.value() == 99);
    }
}

TEST("optional - map")
{
    SECTION("map on empty optional")
    {
        auto const opt = cc::optional<int>{};
        auto result = opt.map([](int x) { return x * 2; });
        static_assert(std::is_same_v<decltype(result), cc::optional<int>>);
        CHECK(!result.has_value());
    }

    SECTION("map on filled optional")
    {
        auto const opt = cc::optional<int>{42};
        auto result = opt.map([](int x) { return x * 2; });
        CHECK(result.has_value());
        CHECK(result.value() == 84);
    }

    SECTION("map with type conversion")
    {
        auto const opt = cc::optional<int>{42};
        auto result = opt.map([](int x) { return std::to_string(x); });
        static_assert(std::is_same_v<decltype(result), cc::optional<std::string>>);
        CHECK(result.has_value());
        CHECK(result.value() == "42");
    }

    SECTION("map empty with type conversion")
    {
        auto const opt = cc::optional<int>{};
        auto result = opt.map([](int x) { return std::to_string(x); });
        static_assert(std::is_same_v<decltype(result), cc::optional<std::string>>);
        CHECK(!result.has_value());
    }

    SECTION("map non-trivial type")
    {
        auto const opt = cc::optional<std::string>{"hello"};
        auto result = opt.map([](std::string const& s) { return s.size(); });
        static_assert(std::is_same_v<decltype(result), cc::optional<size_t>>);
        CHECK(result.has_value());
        CHECK(result.value() == 5);
    }

    SECTION("map with const reference")
    {
        auto const opt = cc::optional<std::string>{"hello"};
        auto result = opt.map([](std::string const& s) { return s + " world"; });
        CHECK(result.has_value());
        CHECK(result.value() == "hello world");
    }

    SECTION("map with mutable reference")
    {
        auto opt = cc::optional<std::string>{"hello"};
        auto result = opt.map(
            [](std::string& s)
            {
                s += " modified";
                return s.size();
            });
        CHECK(result.has_value());
        CHECK(result.value() == 14);
        CHECK(opt.value() == "hello modified");
    }

    SECTION("map with rvalue reference - move")
    {
        auto opt = cc::optional<std::string>{"hello"};
        auto result = cc::move(opt).map([](std::string&& s) { return s + " world"; });
        CHECK(result.has_value());
        CHECK(result.value() == "hello world");
    }

    SECTION("map with move-only type")
    {
        auto opt = cc::optional<move_only>{move_only{42}};
        auto result = cc::move(opt).map([](move_only&& m) { return m.value * 2; });
        static_assert(std::is_same_v<decltype(result), cc::optional<int>>);
        CHECK(result.has_value());
        CHECK(result.value() == 84);
    }

    SECTION("map chaining")
    {
        auto const opt = cc::optional<int>{5};
        auto result = opt.map([](int x) { return x * 2; })
                          .map([](int x) { return x + 10; })
                          .map([](int x) { return std::to_string(x); });
        CHECK(result.has_value());
        CHECK(result.value() == "20");
    }

    SECTION("map chaining with empty")
    {
        auto const opt = cc::optional<int>{};
        auto result = opt.map([](int x) { return x * 2; })
                          .map([](int x) { return x + 10; })
                          .map([](int x) { return std::to_string(x); });
        CHECK(!result.has_value());
    }

    SECTION("map with member function pointer")
    {
        struct foo
        {
            int value;
            int get_doubled() const { return value * 2; }
        };

        auto const opt = cc::optional<foo>{foo{42}};
        auto result = opt.map(&foo::get_doubled);
        CHECK(result.has_value());
        CHECK(result.value() == 84);
    }

    SECTION("map with data member pointer")
    {
        struct foo
        {
            int value;
        };

        auto const opt = cc::optional<foo>{foo{42}};
        auto result = opt.map(&foo::value);
        CHECK(result.has_value());
        CHECK(result.value() == 42);
    }

    SECTION("map returns void - results in optional<void>")
    {
        auto opt = cc::optional<int>{42};
        int side_effect = 0;
        auto result = opt.map([&](int x) { side_effect = x * 2; });
        static_assert(std::is_same_v<decltype(result), cc::optional<cc::unit>>);
        CHECK(result.has_value());
        CHECK(side_effect == 84);
    }

    SECTION("map empty returns void")
    {
        auto opt = cc::optional<int>{};
        int side_effect = 0;
        auto result = opt.map([&](int x) { side_effect = x * 2; });
        static_assert(std::is_same_v<decltype(result), cc::optional<cc::unit>>);
        CHECK(!result.has_value());
        CHECK(side_effect == 0);
    }

    SECTION("map preserves const correctness")
    {
        auto const opt = cc::optional<int>{42};
        auto result = opt.map([](int const& x) { return x; });
        CHECK(result.has_value());
        CHECK(result.value() == 42);
    }
}
