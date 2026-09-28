//-----------------------------------------------------------------------------
// The flat price array and its fallback map, one rule per test. The
// differential sweeps cover the combinations; these name each behaviour, so a
// failure says which rule broke.
//-----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "lob/order_book.hpp"

#include <limits>
#include <vector>

using lob::Config;
using lob::Order;
using lob::OrderBook;
using lob::Price;
using lob::Side;
using lob::Trade;

namespace {

//A book whose array holds `levels` prices starting at `base`.
Config window(std::size_t levels, Price base) {
    Config cfg;
    cfg.price_levels = levels;
    cfg.price_base = base;
    return cfg;
}

std::vector<Price> resting_prices(const OrderBook& book) {
    std::vector<Price> out;
    book.for_each_resting([&](const Order& o) {out.push_back(o.price);});
    return out;
}

std::vector<Price> trade_prices(OrderBook& book) {
    std::vector<Price> out;
    book.drain_trades([&](const Trade& t) {out.push_back(t.price);});
    return out;
}

constexpr Price kMax = std::numeric_limits<Price>::max();

}

TEST_CASE("window: centres on the first price that rests") {
    Config cfg;
    cfg.price_levels = 8;
    OrderBook book(cfg);
    CHECK_FALSE(book.price_window_base());

    book.add_limit(1, Side::Buy, 1000, 5);
    REQUIRE(book.price_window_base());
    CHECK(*book.price_window_base() == 996);
    CHECK(book.price_window_levels() == 8);
}

TEST_CASE("window: a market order into an empty book does not place it") {
    OrderBook book;
    book.add_market(1, Side::Buy, 5);
    CHECK_FALSE(book.price_window_base());
}

TEST_CASE("window: an explicit base places it before any order") {
    OrderBook book(window(8, 500));
    REQUIRE(book.price_window_base());
    CHECK(*book.price_window_base() == 500);
}

TEST_CASE("window: centring stops at price zero") {
    Config cfg;
    cfg.price_levels = 8;
    OrderBook book(cfg);
    book.add_limit(1, Side::Buy, 2, 5);
    CHECK(*book.price_window_base() == 0);
    CHECK(book.levels_opened_outside_window() == 0);
}

TEST_CASE("window: centring stops at the largest price") {
    Config cfg;
    cfg.price_levels = 8;
    OrderBook book(cfg);
    book.add_limit(1, Side::Sell, kMax, 5);
    CHECK(*book.price_window_base() == kMax - 7);
    CHECK(book.price_window_levels() == 8);
    CHECK(book.levels_opened_outside_window() == 0);
}

TEST_CASE("window: an explicit base near the largest price is clipped, not wrapped") {
    OrderBook book(window(8, kMax - 2));
    CHECK(book.price_window_levels() == 3);
    book.add_limit(1, Side::Sell, kMax, 5);
    book.add_limit(2, Side::Buy, 10, 5);        //Far below: the map
    CHECK(book.levels_opened_outside_window() == 1);
    CHECK(book.best_ask() == kMax);
    CHECK(book.best_bid() == 10);
}

TEST_CASE("window: a price outside it rests in the map and still trades") {
    OrderBook book(window(8, 1000));
    book.add_limit(1, Side::Sell, 2000, 5);
    CHECK(book.levels_opened_outside_window() == 1);
    CHECK(book.qty_at(Side::Sell, 2000) == 5);

    const auto rep = book.add_limit(2, Side::Buy, 2000, 5);
    CHECK(rep.filled == 5);
    CHECK(trade_prices(book) == std::vector<Price>{2000});
    CHECK(book.empty());
}

TEST_CASE("window: a bid above it is the best bid, until it leaves") {
    OrderBook book(window(8, 1000));
    book.add_limit(1, Side::Buy, 1003, 5);
    book.add_limit(2, Side::Buy, 5000, 5);
    CHECK(book.best_bid() == 5000);
    book.cancel(2);
    CHECK(book.best_bid() == 1003);
}

TEST_CASE("window: an ask below it is the best ask, until it leaves") {
    OrderBook book(window(8, 1000));
    book.add_limit(1, Side::Sell, 1005, 5);
    book.add_limit(2, Side::Sell, 10, 5);
    CHECK(book.best_ask() == 10);
    book.cancel(2);
    CHECK(book.best_ask() == 1005);
}

TEST_CASE("window: emptying the best level scans to the next occupied price") {
    OrderBook book(window(8, 1000));
    book.add_limit(1, Side::Buy, 1006, 5);
    book.add_limit(2, Side::Buy, 1001, 5);
    book.cancel(1);
    CHECK(book.best_bid() == 1001);
    book.cancel(2);
    CHECK_FALSE(book.best_bid());
}

TEST_CASE("window: a sweep walks from the window into the map, best first") {
    OrderBook book(window(8, 1000));
    book.add_limit(1, Side::Sell, 1500, 5);     //Map, above
    book.add_limit(2, Side::Sell, 1007, 5);     //Array
    book.add_limit(3, Side::Sell, 1002, 5);     //Array
    book.add_limit(4, Side::Sell, 999, 5);      //Map, below: the best ask

    const auto rep = book.add_market(5, Side::Buy, 20);
    CHECK(rep.filled == 20);
    CHECK(trade_prices(book) == std::vector<Price>{999, 1002, 1007, 1500});
    CHECK_FALSE(book.best_ask());
}

TEST_CASE("window: bids list best-first across the map and the array") {
    OrderBook book(window(8, 1000));
    book.add_limit(1, Side::Buy, 10, 1);
    book.add_limit(2, Side::Buy, 1002, 1);
    book.add_limit(3, Side::Buy, 5000, 1);
    book.add_limit(4, Side::Buy, 1004, 1);
    CHECK(resting_prices(book) == std::vector<Price>{5000, 1004, 1002, 10});
}

TEST_CASE("window: asks list best-first across the map and the array") {
    OrderBook book(window(8, 1000));
    book.add_limit(1, Side::Sell, 9000, 1);
    book.add_limit(2, Side::Sell, 1006, 1);
    book.add_limit(3, Side::Sell, 5, 1);
    book.add_limit(4, Side::Sell, 1001, 1);
    CHECK(resting_prices(book) == std::vector<Price>{5, 1001, 1006, 9000});
}

TEST_CASE("window: an emptied array level is reused like a new one") {
    OrderBook book(window(8, 1000));
    book.add_limit(1, Side::Buy, 1003, 5);
    book.cancel(1);
    CHECK(book.qty_at(Side::Buy, 1003) == 0);
    book.add_limit(2, Side::Buy, 1003, 7);
    CHECK(book.qty_at(Side::Buy, 1003) == 7);
    CHECK(book.order_count_at(Side::Buy, 1003) == 1);
    CHECK(book.best_bid() == 1003);
}

TEST_CASE("window: price_levels = 0 files every level in the map") {
    Config cfg;
    cfg.price_levels = 0;
    OrderBook book(cfg);
    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(2, Side::Sell, 101, 5);
    CHECK(book.levels_opened_outside_window() == 2);
    CHECK(book.best_bid() == 100);
    CHECK(book.best_ask() == 101);
    book.add_market(3, Side::Buy, 5);
    CHECK(trade_prices(book) == std::vector<Price>{101});
}
