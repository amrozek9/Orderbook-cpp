#include <catch2/catch_test_macros.hpp>
#include "lob/order_book.hpp"

using lob::OrderBook;
using lob::Side;

TEST_CASE("New book is empty") {
    OrderBook book;
    REQUIRE(book.empty());
    REQUIRE_FALSE(book.best_bid().has_value());
    REQUIRE_FALSE(book.best_ask().has_value());
}

//1. Rest without matching, then query top of book.
TEST_CASE("Resting order that never matches") {
    OrderBook book;

    auto rep = book.add_limit(1, Side::Buy, 100, 10);
    REQUIRE(rep.trades.empty());
    REQUIRE(rep.filled == 0);
    REQUIRE(rep.remaining == 10);
    REQUIRE(rep.rested);

    REQUIRE(book.best_bid() == 100);
    REQUIRE_FALSE(book.best_ask().has_value());
    REQUIRE(book.qty_at(Side::Buy, 100) == 10);
    REQUIRE(book.size() == 1);

    //An ask above the bid does not cross either.
    auto ask = book.add_limit(2, Side::Sell, 101, 5);
    REQUIRE(ask.trades.empty());
    REQUIRE(ask.rested);

    REQUIRE(book.best_bid() == 100);
    REQUIRE(book.best_ask() == 101);
    REQUIRE(book.qty_at(Side::Sell, 101) == 5);
}

//2. Cancel by id.
TEST_CASE("Cancel by id") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);
    book.add_limit(2, Side::Buy, 100, 7);
    book.add_limit(3, Side::Buy, 99, 4);

    REQUIRE(book.cancel(1));
    REQUIRE(book.qty_at(Side::Buy, 100) == 7);
    REQUIRE(book.order_count_at(Side::Buy, 100) == 1);
    REQUIRE(book.best_bid() == 100);
    REQUIRE(book.find(1) == nullptr);

    REQUIRE_FALSE(book.cancel(1));      //Already gone
    REQUIRE_FALSE(book.cancel(99));     //Never existed

    //Emptying a level removes it, so the top of book falls back.
    REQUIRE(book.cancel(2));
    REQUIRE(book.best_bid() == 99);
    REQUIRE(book.qty_at(Side::Buy, 100) == 0);

    REQUIRE(book.cancel(3));
    REQUIRE(book.empty());
    REQUIRE_FALSE(book.best_bid().has_value());
}

//3. One incoming against one resting, exact quantity.
TEST_CASE("Exact-quantity match clears both orders") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);

    auto rep = book.add_limit(2, Side::Buy, 100, 10);
    REQUIRE(rep.trades.size() == 1);
    REQUIRE(rep.trades[0].maker_id == 1);
    REQUIRE(rep.trades[0].taker_id == 2);
    REQUIRE(rep.trades[0].price == 100);
    REQUIRE(rep.trades[0].qty == 10);
    REQUIRE(rep.filled == 10);
    REQUIRE(rep.remaining == 0);
    REQUIRE_FALSE(rep.rested);

    REQUIRE(book.empty());
}

TEST_CASE("Aggressive price trades at the resting order's price") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);

    //Buyer is willing to pay 105 but the maker's 100 is the print.
    auto rep = book.add_limit(2, Side::Buy, 105, 10);
    REQUIRE(rep.trades.size() == 1);
    REQUIRE(rep.trades[0].price == 100);
    REQUIRE(book.empty());
}

//4. Partial fills on either side.
TEST_CASE("Incoming order is partially filled and rests") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 4);

    auto rep = book.add_limit(2, Side::Buy, 100, 10);
    REQUIRE(rep.filled == 4);
    REQUIRE(rep.remaining == 6);
    REQUIRE(rep.rested);

    REQUIRE_FALSE(book.best_ask().has_value());     //Maker fully consumed
    REQUIRE(book.best_bid() == 100);
    REQUIRE(book.qty_at(Side::Buy, 100) == 6);
    REQUIRE(book.find(2)->qty == 6);
}

TEST_CASE("Resting order is partially filled and stays") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);

    auto rep = book.add_limit(2, Side::Buy, 100, 4);
    REQUIRE(rep.filled == 4);
    REQUIRE(rep.remaining == 0);
    REQUIRE_FALSE(rep.rested);

    REQUIRE(book.best_ask() == 100);
    REQUIRE(book.qty_at(Side::Sell, 100) == 6);
    REQUIRE(book.find(1)->qty == 6);
    REQUIRE(book.size() == 1);
}

//5. Walk a queue at one price, then walk several price levels.
TEST_CASE("Walks a price level in time priority") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 100, 5);
    book.add_limit(3, Side::Sell, 100, 5);
    REQUIRE(book.qty_at(Side::Sell, 100) == 15);

    auto rep = book.add_limit(4, Side::Buy, 100, 12);
    REQUIRE(rep.filled == 12);
    REQUIRE(rep.trades.size() == 3);
    REQUIRE(rep.trades[0].maker_id == 1);       //Oldest first
    REQUIRE(rep.trades[1].maker_id == 2);
    REQUIRE(rep.trades[2].maker_id == 3);
    REQUIRE(rep.trades[2].qty == 2);            //Partial on the last maker

    REQUIRE(book.qty_at(Side::Sell, 100) == 3);
    REQUIRE(book.order_count_at(Side::Sell, 100) == 1);
    REQUIRE(book.find(3)->qty == 3);
}

TEST_CASE("Sweeps multiple price levels in price order") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 102, 5);
    book.add_limit(2, Side::Sell, 100, 5);
    book.add_limit(3, Side::Sell, 101, 5);

    auto rep = book.add_limit(4, Side::Buy, 102, 13);
    REQUIRE(rep.filled == 13);
    REQUIRE(rep.remaining == 0);
    REQUIRE(rep.trades.size() == 3);
    REQUIRE(rep.trades[0].price == 100);        //Cheapest first
    REQUIRE(rep.trades[1].price == 101);
    REQUIRE(rep.trades[2].price == 102);
    REQUIRE(rep.trades[2].qty == 3);

    REQUIRE(book.best_ask() == 102);
    REQUIRE(book.qty_at(Side::Sell, 102) == 2);
}

TEST_CASE("Stops at the limit price and rests the rest") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 103, 5);      //Too expensive for the taker

    auto rep = book.add_limit(3, Side::Buy, 101, 8);
    REQUIRE(rep.filled == 5);
    REQUIRE(rep.remaining == 3);
    REQUIRE(rep.rested);

    REQUIRE(book.best_bid() == 101);
    REQUIRE(book.qty_at(Side::Buy, 101) == 3);
    REQUIRE(book.best_ask() == 103);
}

TEST_CASE("Selling sweeps bids from the highest price down") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 99, 5);
    book.add_limit(2, Side::Buy, 101, 5);
    book.add_limit(3, Side::Buy, 100, 5);

    auto rep = book.add_limit(4, Side::Sell, 100, 8);
    REQUIRE(rep.trades.size() == 2);
    REQUIRE(rep.trades[0].price == 101);        //Best bid first
    REQUIRE(rep.trades[1].price == 100);
    REQUIRE(rep.filled == 8);

    REQUIRE(book.best_bid() == 100);
    REQUIRE(book.qty_at(Side::Buy, 100) == 2);
    REQUIRE(book.qty_at(Side::Buy, 99) == 5);
    REQUIRE_FALSE(book.best_ask().has_value());     //Nothing rested
}

//6. Market orders, including running out of book.
TEST_CASE("Market order ignores price and never rests") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 250, 5);

    auto rep = book.add_market(3, Side::Buy, 8);
    REQUIRE(rep.filled == 8);
    REQUIRE(rep.remaining == 0);
    REQUIRE_FALSE(rep.rested);
    REQUIRE(rep.trades[0].price == 100);
    REQUIRE(rep.trades[1].price == 250);        //Pays up without a limit

    REQUIRE(book.qty_at(Side::Sell, 250) == 2);
}

TEST_CASE("Market order that exhausts the book drops the remainder") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 3);
    book.add_limit(2, Side::Sell, 101, 2);

    auto rep = book.add_market(3, Side::Buy, 10);
    REQUIRE(rep.filled == 5);
    REQUIRE(rep.remaining == 5);            //Unfilled, and cancelled
    REQUIRE_FALSE(rep.rested);

    REQUIRE(book.empty());
    REQUIRE_FALSE(book.best_ask().has_value());
    REQUIRE(book.find(3) == nullptr);
}

TEST_CASE("Market order against an empty book does nothing") {
    OrderBook book;
    auto rep = book.add_market(1, Side::Buy, 10);
    REQUIRE(rep.trades.empty());
    REQUIRE(rep.filled == 0);
    REQUIRE(rep.remaining == 10);
    REQUIRE(book.empty());

    //A one-sided book is still empty for the taker's purposes.
    book.add_limit(2, Side::Buy, 100, 5);
    auto buy = book.add_market(3, Side::Buy, 5);
    REQUIRE(buy.filled == 0);
    REQUIRE(buy.remaining == 5);
    REQUIRE(book.size() == 1);
}

//7. Modify as cancel plus add.
TEST_CASE("Modify moves price and quantity, keeping the id") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);

    auto rep = book.modify(1, 99, 4);
    REQUIRE(rep.accepted);
    REQUIRE(rep.trades.empty());
    REQUIRE(rep.rested);

    REQUIRE(book.qty_at(Side::Buy, 100) == 0);
    REQUIRE(book.best_bid() == 99);
    REQUIRE(book.qty_at(Side::Buy, 99) == 4);
    REQUIRE(book.find(1)->qty == 4);
    REQUIRE(book.size() == 1);
}

TEST_CASE("Modify loses time priority") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 100, 5);

    book.modify(1, 100, 5);     //Same price, but goes to the back of the queue

    auto rep = book.add_limit(3, Side::Buy, 100, 10);
    REQUIRE(rep.trades.size() == 2);
    REQUIRE(rep.trades[0].maker_id == 2);
    REQUIRE(rep.trades[1].maker_id == 1);
}

TEST_CASE("Modify into a crossing price trades immediately") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 101, 5);
    book.add_limit(2, Side::Buy, 100, 5);

    auto rep = book.modify(2, 101, 5);      //Bid up through the ask
    REQUIRE(rep.trades.size() == 1);
    REQUIRE(rep.trades[0].maker_id == 1);
    REQUIRE(rep.trades[0].taker_id == 2);
    REQUIRE(rep.trades[0].price == 101);
    REQUIRE(rep.filled == 5);
    REQUIRE_FALSE(rep.rested);
    REQUIRE(book.empty());
}

TEST_CASE("Modify to zero quantity is a cancel, and unknown ids are rejected") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);

    auto rep = book.modify(1, 100, 0);
    REQUIRE(rep.accepted);
    REQUIRE(book.empty());

    auto missing = book.modify(42, 100, 5);
    REQUIRE_FALSE(missing.accepted);
    REQUIRE(missing.remaining == 5);
    REQUIRE(book.empty());
}

TEST_CASE("Duplicate live ids are rejected") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);

    auto dup = book.add_limit(1, Side::Buy, 99, 5);
    REQUIRE_FALSE(dup.accepted);
    REQUIRE(dup.remaining == 5);
    REQUIRE(book.size() == 1);
    REQUIRE(book.qty_at(Side::Buy, 100) == 10);
    REQUIRE(book.qty_at(Side::Buy, 99) == 0);
}
