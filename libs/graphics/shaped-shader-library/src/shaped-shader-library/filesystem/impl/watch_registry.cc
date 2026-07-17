#include <shaped-shader-library/filesystem/impl/path.hh>
#include <shaped-shader-library/filesystem/impl/watch_registry.hh>

void slib::impl::watch_slot::fire() const
{
    _sink.lock(
        [](watch_sink const& sink)
        {
            if (sink)
                sink();
        });
}

void slib::impl::watch_slot::cancel()
{
    _sink.lock([](watch_sink& sink) { sink = watch_sink(); });
}

slib::watch_subscription slib::impl::watch_registry::add(cc::string prefix, watch_sink sink)
{
    auto slot = std::make_shared<watch_slot>(cc::move(sink));

    _entries.lock(
        [&](cc::vector<entry>& entries)
        {
            // Registering is the only thing that ever touches the list, so it is also the only place that
            // can clear out what dropped subscriptions left behind.
            entries.remove_all_where([](entry const& e) { return e.slot.expired(); });
            entries.push_back(entry{.prefix = cc::move(prefix), .slot = slot});
        });

    return watch_subscription(std::make_unique<slot_subscription>(cc::move(slot)));
}

void slib::impl::watch_registry::fire_for(cc::string_view path) const
{
    // Collect first, fire after: a sink is arbitrary code and must not run under our lock.
    cc::vector<std::shared_ptr<watch_slot>> to_fire;
    _entries.lock(
        [&](cc::vector<entry> const& entries)
        {
            for (auto const& e : entries)
                if (is_path_under(path, e.prefix))
                    if (auto slot = e.slot.lock(); slot != nullptr)
                        to_fire.push_back(cc::move(slot));
        });

    for (auto const& slot : to_fire)
        slot->fire();
}
