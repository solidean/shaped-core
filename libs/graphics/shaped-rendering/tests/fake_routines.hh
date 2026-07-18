#pragma once

#include <shaped-rendering/render_routine.hh>
#include <shaped-rendering/render_routine_package.hh>

#include <memory>

// Fake routines and packages shared by the shaped-rendering tests. They exercise the framework's
// orchestration (registration, dependency closure, dedup, cycle detection, phase re-init) without any
// GPU work — the counting routine ignores the context and only records how often each phase ran.

namespace sr_test
{
/// Records how often each init phase ran; does no GPU work.
struct counting_routine : sr::render_routine
{
    int once = 0;
    int declare = 0;
    int materialize = 0;

protected:
    void init_once(sg::context&) override { ++once; }
    void init_declare(sg::context&) override { ++declare; }
    void init_materialize(sg::command_list&) override { ++materialize; }
};

/// One routine, no dependencies. Singleton by the customary convention.
struct leaf_package : sr::render_routine_package
{
    static std::shared_ptr<leaf_package> acquire()
    {
        static auto instance = sr::make_package<leaf_package>();
        return instance;
    }
    sr::routine_handle<counting_routine> routine;

protected:
    void setup() override { routine = register_routine<counting_routine>(); }
};

/// Depends on the leaf singleton and keeps a handle to it.
struct dependent_package : sr::render_routine_package
{
    static std::shared_ptr<dependent_package> acquire()
    {
        static auto instance = sr::make_package<dependent_package>();
        return instance;
    }
    std::shared_ptr<leaf_package> leaf;
    sr::routine_handle<counting_routine> routine;

protected:
    void setup() override
    {
        leaf = leaf_package::acquire();
        depend(leaf);
        routine = register_routine<counting_routine>();
    }
};

// Diamond: a depends on b and c, both of which depend on the d singleton — so a->b->d == a->c->d.
struct diamond_d : sr::render_routine_package
{
    static std::shared_ptr<diamond_d> acquire()
    {
        static auto instance = sr::make_package<diamond_d>();
        return instance;
    }
    sr::routine_handle<counting_routine> routine;

protected:
    void setup() override { routine = register_routine<counting_routine>(); }
};
struct diamond_b : sr::render_routine_package
{
    static std::shared_ptr<diamond_b> acquire()
    {
        static auto instance = sr::make_package<diamond_b>();
        return instance;
    }
    std::shared_ptr<diamond_d> d;
    sr::routine_handle<counting_routine> routine;

protected:
    void setup() override
    {
        d = diamond_d::acquire();
        depend(d);
        routine = register_routine<counting_routine>();
    }
};
struct diamond_c : sr::render_routine_package
{
    static std::shared_ptr<diamond_c> acquire()
    {
        static auto instance = sr::make_package<diamond_c>();
        return instance;
    }
    std::shared_ptr<diamond_d> d;
    sr::routine_handle<counting_routine> routine;

protected:
    void setup() override
    {
        d = diamond_d::acquire();
        depend(d);
        routine = register_routine<counting_routine>();
    }
};
struct diamond_a : sr::render_routine_package
{
    std::shared_ptr<diamond_b> b;
    std::shared_ptr<diamond_c> c;
    sr::routine_handle<counting_routine> routine;

protected:
    void setup() override
    {
        b = diamond_b::acquire();
        c = diamond_c::acquire();
        depend(b);
        depend(c);
        routine = register_routine<counting_routine>();
    }
};

// Cyclic dependency: cycle_a depends on cycle_b, which depends back on cycle_a. Resolved through
// make_package (not a static singleton, which would deadlock in static init), so the guard asserts.
struct cycle_b;
struct cycle_a : sr::render_routine_package
{
protected:
    void setup() override;
};
struct cycle_b : sr::render_routine_package
{
protected:
    void setup() override;
};
inline void cycle_a::setup()
{
    depend(sr::make_package<cycle_b>());
}
inline void cycle_b::setup()
{
    depend(sr::make_package<cycle_a>());
}
} // namespace sr_test
