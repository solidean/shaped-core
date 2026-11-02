#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
struct order
{
    int customer;
    cc::vector<int> item_ids;
};

struct item
{
    cc::string_view name;
    float price;
    int stock;
};

cc::vector<item> make_catalog()
{
    return {{"bolt", 0.25f, 400}, {"nut", 0.1f, 1000}, {"hinge", 3.5f, 12}, {"bracket", 2.0f, 0}, {"spring", 1.25f, 60}};
}

// Reads the day's orders; returned by value, as an importer would.
cc::vector<order> load_orders()
{
    return {{7, {0, 0, 2}}, {3, {1, 4}}, {7, {3}}, {9, {2, 2, 2, 4}}};
}
} // namespace

EXAMPLE("clean-core/sequence-pipelines")
{
    auto const catalog = make_catalog();

    // zip walks several sequences in step and hands each callback the items spread out, as separate arguments.
    // Here a price list and a quantity list become a stock valuation without an index in sight.
    auto const prices = catalog.values().map(&item::price).to_vector();
    auto const stock = catalog.values().map(&item::stock).to_vector();
    cc::println("stock value: {}",
                cc::zip(prices, stock).map([](float p, int n) { return p * float(n); }).sum_from(0.0f));

    // Projections: sum(f), max(f), average(f) reduce a member, and min_by / max_by keep the whole element.
    cc::println("priciest item: {}", catalog.values().assert_non_empty().max_by(&item::price).name);
    cc::println("mean price:    {}", catalog.values().assert_non_empty().average(&item::price));

    // enumerate pairs each element with its position, which is how an id that is an index is recovered.
    cc::println("out of stock:");
    catalog.values()
        .enumerate()
        .filter([](isize, item const& it) { return it.stock == 0; })
        .each([](isize id, item const& it) { cc::println("  #{} {}", id, it.name); });

    // A temporary container is owned by its sequence, so an importer's result is processed without a variable.
    // flat_map turns each order into its item ids; the vector of ids belongs to the order and is walked in place.
    auto const units = load_orders().values().flat_map([](order const& o) { return o.item_ids.values(); }).count();
    cc::println("units ordered: {}", units);

    // Grouping into a map, then reading it back through keys() and values().
    auto const orders = load_orders();
    auto revenue_by_customer = cc::map<int, float>();
    orders.values().each(
        [&](order const& o)
        {
            revenue_by_customer[o.customer]
                += o.item_ids.values().map([&](int id) { return catalog[id].price; }).sum_from(0.0f);
        });

    cc::println("customers: {}", revenue_by_customer.keys().count());
    cc::println("customer 7 ordered: {}", revenue_by_customer.keys().contains(7)); // the map's lookup, not a scan
    cc::println("total revenue: {}", revenue_by_customer.values().sum_from(0.0f));

    // keys() and values() of one map walk in the same order, so zipping them gives the entries.
    cc::println("big spenders:");
    for (auto&& [customer, revenue] : cc::zip(revenue_by_customer.keys(), revenue_by_customer.values()))
        if (revenue > 5.0f)
            cc::println("  customer {}: {}", customer, revenue);

    // pairwise walks neighbours, spread into the callback like a zip: the day-over-day change.
    auto const daily = cc::vector<int>{12, 15, 11, 18, 21, 19};
    cc::println("day-over-day: {}", daily.values().pairwise().map([](int a, int b) { return b - a; }));
    cc::println("best gain: {}",
                daily.values().pairwise().map([](int a, int b) { return b - a; }).assert_non_empty().max());

    // chunks groups without overlap: the orders as (customer, item) records packed flat.
    auto const flat = cc::vector<int>{7, 0, 3, 1, 7, 3, 9, 2};
    flat.values().chunks<2>().each([&](int customer, int id)
                                   { cc::println("  customer {} took a {}", customer, catalog[id].name); });

    // count_by is a histogram, partition splits in two, and min_max finds both ends in one pass.
    auto const per_customer = flat.values().step_by(2).count_by([](int customer) { return customer; });
    cc::println("customer 7 placed {} orders", per_customer.get(7));
    auto const [cheap, dear] = catalog.values().partition([](item const& it) { return it.price < 1.0f; });
    cc::println("{} cheap items, {} dear ones", cheap.size(), dear.size());
    auto const [low, high] = catalog.values().map(&item::price).assert_non_empty().min_max();
    cc::println("prices span {} to {}", low, high);

    // take_while / skip_while cut at the first element that fails, rather than filtering every one.
    auto const daily_counts = cc::vector<int>{0, 0, 0, 4, 9, 2, 0, 7};
    auto const from_first_active = [&] { return daily_counts.values().skip_while([](int c) { return c == 0; }); };
    cc::println("from the first active day: {} days, {} orders", from_first_active().count(),
                from_first_active().sum_from(0));
    cc::println("opening streak: {}", daily_counts.values().skip(3).take_while([](int c) { return c > 0; }).count());

    // chain appends one sequence to another of the same element type.
    auto const backorders = cc::vector<int>{2, 3};
    cc::println("ids to restock: {}",
                orders.values().flat_map([](order const& o) { return o.item_ids.values(); }).chain(backorders).count());

    // count over a sized map is answered without calling the map's function.
    // consume() is the terminal operation that runs every element through, for a chain kept for its side effects.
    int logged = 0;
    catalog.values()
        .map(
            [&](item const& it)
            {
                ++logged;
                return it.stock;
            })
        .consume();
    cc::println("audited {} items", logged);
}
