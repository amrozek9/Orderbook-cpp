#include <catch2/catch_test_macros.hpp>
#include "lob/order_book.hpp"

#include <vector>

using lob::Config;
using lob::OrderBook;
using lob::SelfTradePolicy;
using lob::Side;
using lob::Trade;

namespace {

//Trades leave through the ring; tests pull them out here.
std::vector<Trade> drain(OrderBook& book) {
    std::vector<Trade> out;
    book.drain_trades([&](const Trade& t) {out.push_back(t);});
    return out;
}

bool identical(const Trade& a, const Trade& b) {
    return a.seq == b.seq && a.maker_id == b.maker_id && a.taker_id == b.taker_id &&
           a.price == b.price && a.qty == b.qty && a.taker_side == b.taker_side;
}

}

TEST_CASE("New book is empty") {
    OrderBook book;
    REQUIRE(book.empty());
    REQUIRE_FALSE(book.best_bid().has_value());
    REQUIRE_FALSE(book.best_ask().has_value());
    REQUIRE(book.trades().empty());
    book.check_invariants();
}

//1. Rest without matching, then query top of book.
TEST_CASE("Resting order that never matches") {
    OrderBook book;

    auto rep = book.add_limit(1, Side::Buy, 100, 10);
    REQUIRE(rep.trade_count == 0);
    REQUIRE(rep.first_seq == 0);
    REQUIRE(rep.filled == 0);
    REQUIRE(rep.remaining == 10);
    REQUIRE(rep.rested);
    REQUIRE(book.trades().empty());

    REQUIRE(book.best_bid() == 100);
    REQUIRE_FALSE(book.best_ask().has_value());
    REQUIRE(book.qty_at(Side::Buy, 100) == 10);
    REQUIRE(book.size() == 1);

    //An ask above the bid does not cross either.
    auto ask = book.add_limit(2, Side::Sell, 101, 5);
    REQUIRE(ask.trade_count == 0);
    REQUIRE(ask.rested);

    REQUIRE(book.best_bid() == 100);
    REQUIRE(book.best_ask() == 101);
    REQUIRE(book.qty_at(Side::Sell, 101) == 5);
    book.check_invariants();
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
    book.check_invariants();
}

//3. One incoming against one resting, exact quantity.
TEST_CASE("Exact-quantity match clears both orders") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);

    auto rep = book.add_limit(2, Side::Buy, 100, 10);
    REQUIRE(rep.trade_count == 1);
    REQUIRE(rep.filled == 10);
    REQUIRE(rep.remaining == 0);
    REQUIRE_FALSE(rep.rested);

    auto fills = drain(book);
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].seq == rep.first_seq);
    REQUIRE(fills[0].maker_id == 1);
    REQUIRE(fills[0].taker_id == 2);
    REQUIRE(fills[0].price == 100);
    REQUIRE(fills[0].qty == 10);
    REQUIRE(fills[0].taker_side == Side::Buy);

    REQUIRE(book.empty());
}

TEST_CASE("Aggressive price trades at the resting order's price") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);

    //Buyer is willing to pay 105 but the maker's 100 is the print.
    auto rep = book.add_limit(2, Side::Buy, 105, 10);
    REQUIRE(rep.trade_count == 1);
    REQUIRE(drain(book)[0].price == 100);
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
    book.check_invariants();
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
    book.check_invariants();
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
    REQUIRE(rep.trade_count == 3);

    auto fills = drain(book);
    REQUIRE(fills[0].maker_id == 1);        //Oldest first
    REQUIRE(fills[1].maker_id == 2);
    REQUIRE(fills[2].maker_id == 3);
    REQUIRE(fills[2].qty == 2);             //Partial on the last maker
    REQUIRE(fills[0].seq + 1 == fills[1].seq);      //Seq is contiguous per order
    REQUIRE(fills[1].seq + 1 == fills[2].seq);

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
    REQUIRE(rep.trade_count == 3);

    auto fills = drain(book);
    REQUIRE(fills[0].price == 100);         //Cheapest first
    REQUIRE(fills[1].price == 101);
    REQUIRE(fills[2].price == 102);
    REQUIRE(fills[2].qty == 3);

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
    book.check_invariants();
}

TEST_CASE("Selling sweeps bids from the highest price down") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 99, 5);
    book.add_limit(2, Side::Buy, 101, 5);
    book.add_limit(3, Side::Buy, 100, 5);

    auto rep = book.add_limit(4, Side::Sell, 100, 8);
    REQUIRE(rep.trade_count == 2);
    REQUIRE(rep.filled == 8);

    auto fills = drain(book);
    REQUIRE(fills[0].price == 101);         //Best bid first
    REQUIRE(fills[1].price == 100);
    REQUIRE(fills[0].taker_side == Side::Sell);

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

    auto fills = drain(book);
    REQUIRE(fills[0].price == 100);
    REQUIRE(fills[1].price == 250);         //Pays up without a limit

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
    book.check_invariants();
}

TEST_CASE("Market order against an empty book does nothing") {
    OrderBook book;
    auto rep = book.add_market(1, Side::Buy, 10);
    REQUIRE(rep.trade_count == 0);
    REQUIRE(rep.filled == 0);
    REQUIRE(rep.remaining == 10);
    REQUIRE(book.empty());
    REQUIRE(book.trades().empty());

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
    REQUIRE(rep.trade_count == 0);
    REQUIRE(rep.rested);

    REQUIRE(book.qty_at(Side::Buy, 100) == 0);
    REQUIRE(book.best_bid() == 99);
    REQUIRE(book.qty_at(Side::Buy, 99) == 4);
    REQUIRE(book.find(1)->qty == 4);
    REQUIRE(book.size() == 1);
    book.check_invariants();
}

TEST_CASE("Modify loses time priority") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 100, 5);

    book.modify(1, 100, 5);     //Same price, but goes to the back of the queue

    auto rep = book.add_limit(3, Side::Buy, 100, 10);
    REQUIRE(rep.trade_count == 2);

    auto fills = drain(book);
    REQUIRE(fills[0].maker_id == 2);
    REQUIRE(fills[1].maker_id == 1);
}

TEST_CASE("Modify into a crossing price trades immediately") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 101, 5);
    book.add_limit(2, Side::Buy, 100, 5);

    auto rep = book.modify(2, 101, 5);      //Bid up through the ask
    REQUIRE(rep.trade_count == 1);
    REQUIRE(rep.filled == 5);
    REQUIRE_FALSE(rep.rested);

    auto fills = drain(book);
    REQUIRE(fills[0].maker_id == 1);
    REQUIRE(fills[0].taker_id == 2);
    REQUIRE(fills[0].price == 101);
    REQUIRE(book.empty());
}

TEST_CASE("Modify preserves the owner for self-trade purposes") {
    OrderBook book;
    book.add_limit(1, /*owner=*/7, Side::Sell, 101, 5);
    book.add_limit(2, /*owner=*/7, Side::Buy, 100, 5);

    auto rep = book.modify(2, 101, 5);      //Would be a wash trade
    REQUIRE(rep.trade_count == 0);
    REQUIRE(rep.stp_cancelled == 5);
    REQUIRE(book.find(1) == nullptr);       //Resting side was cancelled
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
    book.check_invariants();
}

//--- trade output path -------------------------------------------------------

TEST_CASE("Trades go to the ring, in order, and survive until drained") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 101, 5);

    book.add_limit(3, Side::Buy, 101, 4);       //Fill 1
    book.add_limit(4, Side::Buy, 101, 6);       //Fills 2 and 3
    REQUIRE(book.trades().size() == 3);         //Nothing drained yet

    auto fills = drain(book);
    REQUIRE(fills.size() == 3);
    REQUIRE(fills[0].seq == 1);                 //Stream-wide monotonic counter
    REQUIRE(fills[1].seq == 2);
    REQUIRE(fills[2].seq == 3);
    REQUIRE(fills[0].taker_id == 3);
    REQUIRE(fills[1].taker_id == 4);
    REQUIRE(fills[2].taker_id == 4);
    REQUIRE(book.trades().empty());
    REQUIRE(book.trades().dropped() == 0);
}

TEST_CASE("Ring overflow is counted, not silent") {
    OrderBook book(Config{SelfTradePolicy::CancelResting, /*trade_capacity=*/2});
    REQUIRE(book.trades().capacity() == 2);

    book.add_limit(1, Side::Sell, 100, 1);
    book.add_limit(2, Side::Sell, 100, 1);
    book.add_limit(3, Side::Sell, 100, 1);
    book.add_limit(4, Side::Sell, 100, 1);

    auto rep = book.add_limit(5, Side::Buy, 100, 4);
    REQUIRE(rep.trade_count == 4);              //The book still matched all four
    REQUIRE(rep.filled == 4);
    REQUIRE(book.trades().size() == 2);
    REQUIRE(book.trades().dropped() == 2);      //Caller sized the ring too small
    book.check_invariants();
}

TEST_CASE("Ring wraps around its capacity") {
    lob::TradeRing ring(4);
    Trade out{};
    for (lob::Sequence s = 1; s <= 12; ++s) {
        REQUIRE(ring.push(Trade{s, 1, 2, 100, 1, Side::Buy}));
        REQUIRE(ring.pop(out));
        REQUIRE(out.seq == s);
    }
    REQUIRE(ring.empty());
    REQUIRE(ring.dropped() == 0);
    REQUIRE_FALSE(ring.pop(out));
}

//--- determinism / replay ----------------------------------------------------

TEST_CASE("The same input sequence produces the same trades, every run") {
    const auto replay = [](OrderBook& book) {
        book.add_limit(1, 11, Side::Sell, 102, 5);
        book.add_limit(2, 12, Side::Sell, 100, 5);
        book.add_limit(3, 11, Side::Sell, 101, 5);
        book.add_limit(4, 12, Side::Buy, 99, 7);
        book.modify(4, 103, 12);
        book.add_market(5, 13, Side::Sell, 4);
        book.cancel(3);
        book.add_limit(6, 13, Side::Buy, 104, 9);
        book.add_market(7, 11, Side::Buy, 100);     //Runs the book dry
    };

    OrderBook first, second;
    replay(first);
    replay(second);

    const auto a = drain(first);
    const auto b = drain(second);
    REQUIRE_FALSE(a.empty());
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(identical(a[i], b[i]));

    //Book state matches too, not just the trade stream.
    REQUIRE(first.size() == second.size());
    REQUIRE(first.best_bid() == second.best_bid());
    REQUIRE(first.best_ask() == second.best_ask());
    first.check_invariants();
    second.check_invariants();
}

TEST_CASE("Trade sequence numbers restart per book, not per process") {
    OrderBook a, b;
    a.add_limit(1, Side::Sell, 100, 1);
    a.add_limit(2, Side::Buy, 100, 1);
    b.add_limit(1, Side::Sell, 100, 1);
    b.add_limit(2, Side::Buy, 100, 1);

    REQUIRE(drain(a)[0].seq == 1);
    REQUIRE(drain(b)[0].seq == 1);      //No shared global counter
}

//--- self-trade prevention ---------------------------------------------------

TEST_CASE("Anonymous orders never trigger self-trade prevention") {
    OrderBook book;     //Both sides default to kAnonymous
    book.add_limit(1, Side::Sell, 100, 5);

    auto rep = book.add_limit(2, Side::Buy, 100, 5);
    REQUIRE(rep.trade_count == 1);
    REQUIRE(rep.stp_cancelled == 0);
    REQUIRE(book.empty());
}

TEST_CASE("CancelResting kills the maker and lets the taker keep walking") {
    OrderBook book;     //Default policy
    REQUIRE(book.self_trade_policy() == SelfTradePolicy::CancelResting);
    book.add_limit(1, /*owner=*/7, Side::Sell, 100, 5);     //Same owner as the taker
    book.add_limit(2, /*owner=*/8, Side::Sell, 100, 5);
    book.add_limit(3, /*owner=*/8, Side::Sell, 101, 5);

    auto rep = book.add_limit(4, /*owner=*/7, Side::Buy, 101, 12);
    REQUIRE(rep.stp_cancelled == 5);            //Order 1 removed, no print
    REQUIRE_FALSE(rep.stp_halted);
    REQUIRE(rep.filled == 10);                  //Still filled by orders 2 and 3
    REQUIRE(rep.remaining == 2);
    REQUIRE(rep.rested);

    auto fills = drain(book);
    REQUIRE(fills.size() == 2);
    for (const Trade& t : fills) REQUIRE(t.maker_id != 1);

    REQUIRE(book.find(1) == nullptr);
    REQUIRE(book.best_bid() == 101);            //Leftover rested
    book.check_invariants();
}

TEST_CASE("CancelIncoming stops the taker and leaves the book untouched") {
    OrderBook book(Config{SelfTradePolicy::CancelIncoming, 4096});
    book.add_limit(1, /*owner=*/8, Side::Sell, 100, 5);
    book.add_limit(2, /*owner=*/7, Side::Sell, 101, 5);     //Same owner as the taker

    auto rep = book.add_limit(3, /*owner=*/7, Side::Buy, 101, 12);
    REQUIRE(rep.filled == 5);                   //Traded with order 1 first
    REQUIRE(rep.stp_halted);
    REQUIRE(rep.stp_cancelled == 0);            //Nothing resting was removed
    REQUIRE(rep.remaining == 7);                //Cancelled, and reported as such
    REQUIRE_FALSE(rep.rested);                  //remaining>0 + !rested == dropped

    REQUIRE(book.find(2) != nullptr);           //Maker survives untouched
    REQUIRE(book.qty_at(Side::Sell, 101) == 5);
    REQUIRE_FALSE(book.best_bid().has_value());
    book.check_invariants();
}

TEST_CASE("Allow prints the wash trade") {
    OrderBook book(Config{SelfTradePolicy::Allow, 4096});
    book.add_limit(1, /*owner=*/7, Side::Sell, 100, 5);

    auto rep = book.add_limit(2, /*owner=*/7, Side::Buy, 100, 5);
    REQUIRE(rep.trade_count == 1);
    REQUIRE(rep.filled == 5);
    REQUIRE(rep.stp_cancelled == 0);
    REQUIRE_FALSE(rep.stp_halted);

    auto fills = drain(book);
    REQUIRE(fills[0].maker_id == 1);
    REQUIRE(fills[0].taker_id == 2);
    REQUIRE(book.empty());
}

TEST_CASE("Self-trade prevention only fires against the same owner") {
    OrderBook book;
    book.add_limit(1, /*owner=*/7, Side::Sell, 100, 5);

    auto rep = book.add_limit(2, /*owner=*/9, Side::Buy, 100, 5);
    REQUIRE(rep.trade_count == 1);
    REQUIRE(rep.stp_cancelled == 0);
    REQUIRE(book.empty());
}

TEST_CASE("CancelResting can empty a level and move the top of book") {
    OrderBook book;
    book.add_limit(1, /*owner=*/7, Side::Sell, 100, 5);
    book.add_limit(2, /*owner=*/7, Side::Sell, 100, 5);
    book.add_limit(3, /*owner=*/8, Side::Sell, 102, 5);

    auto rep = book.add_limit(4, /*owner=*/7, Side::Buy, 101, 5);
    REQUIRE(rep.stp_cancelled == 10);       //Whole level wiped
    REQUIRE(rep.filled == 0);
    REQUIRE(rep.rested);                    //Rests at 101, below the 102 ask

    REQUIRE(book.qty_at(Side::Sell, 100) == 0);
    REQUIRE(book.best_ask() == 102);
    REQUIRE(book.best_bid() == 101);
    REQUIRE(book.trades().empty());         //STP cancels do not print
    book.check_invariants();
}

TEST_CASE("Self-trade prevention against a market order that runs the book dry") {
    OrderBook book;
    book.add_limit(1, /*owner=*/7, Side::Sell, 100, 5);
    book.add_limit(2, /*owner=*/7, Side::Sell, 101, 5);

    auto rep = book.add_market(3, /*owner=*/7, Side::Buy, 10);
    REQUIRE(rep.filled == 0);
    REQUIRE(rep.stp_cancelled == 10);
    REQUIRE(rep.remaining == 10);
    REQUIRE(book.empty());
    book.check_invariants();
}

//--- edge cases --------------------------------------------------------------

TEST_CASE("Zero-quantity orders are rejected, not silently swallowed") {
    OrderBook book;

    auto limit = book.add_limit(1, Side::Buy, 100, 0);
    REQUIRE_FALSE(limit.accepted);
    REQUIRE(limit.filled == 0);
    REQUIRE_FALSE(limit.rested);

    auto market = book.add_market(2, Side::Sell, 0);
    REQUIRE_FALSE(market.accepted);

    REQUIRE(book.empty());          //Neither left a trace
    book.check_invariants();
}

TEST_CASE("A level holds more volume than a 32-bit quantity can express") {
    //Two near-max orders sum past 2^32. The per-order type stays 32-bit; the
    //level aggregate must be wider or it wraps and every total goes wrong.
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 3'000'000'000u);
    book.add_limit(2, Side::Buy, 100, 3'000'000'000u);

    REQUIRE(book.qty_at(Side::Buy, 100) == 6'000'000'000ull);
    REQUIRE(book.order_count_at(Side::Buy, 100) == 2);
    book.check_invariants();        //Would trip if the total had wrapped

    REQUIRE(book.cancel(1));
    REQUIRE(book.qty_at(Side::Buy, 100) == 3'000'000'000ull);
}

TEST_CASE("Price zero and max price are ordinary prices") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 0, 5);
    book.add_limit(2, Side::Sell, UINT64_MAX, 5);

    REQUIRE(book.best_bid() == 0);
    REQUIRE(book.best_ask() == UINT64_MAX);
    REQUIRE(book.qty_at(Side::Buy, 0) == 5);
    book.check_invariants();

    //A market buy still reaches the ask at the top of the range.
    auto rep = book.add_market(3, Side::Buy, 5);
    REQUIRE(rep.filled == 5);
    REQUIRE(drain(book)[0].price == UINT64_MAX);
}

TEST_CASE("A duplicate id is rejected even on the opposite side") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);

    auto dup = book.add_limit(1, Side::Sell, 200, 5);
    REQUIRE_FALSE(dup.accepted);
    REQUIRE(book.size() == 1);
    REQUIRE(book.find(1)->side == Side::Buy);       //Original untouched
    REQUIRE_FALSE(book.best_ask().has_value());
}

TEST_CASE("Self-trade prevention needs an owner on both sides") {
    SECTION("anonymous taker, owned maker") {
        OrderBook book;
        book.add_limit(1, /*owner=*/7, Side::Sell, 100, 5);
        auto rep = book.add_limit(2, lob::kAnonymous, Side::Buy, 100, 5);
        REQUIRE(rep.trade_count == 1);
        REQUIRE(rep.stp_cancelled == 0);
    }
    SECTION("owned taker, anonymous maker") {
        OrderBook book;
        book.add_limit(1, lob::kAnonymous, Side::Sell, 100, 5);
        auto rep = book.add_limit(2, /*owner=*/7, Side::Buy, 100, 5);
        REQUIRE(rep.trade_count == 1);
        REQUIRE(rep.stp_cancelled == 0);
    }
}

TEST_CASE("Exhausting a level exactly continues into the next one") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 101, 5);

    auto rep = book.add_limit(3, Side::Buy, 101, 10);
    REQUIRE(rep.trade_count == 2);
    REQUIRE(rep.filled == 10);
    REQUIRE(book.empty());              //Both levels cleaned up
    REQUIRE_FALSE(book.best_ask().has_value());
    book.check_invariants();
}

TEST_CASE("A level can be emptied and then reused") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);
    REQUIRE(book.cancel(1));
    REQUIRE(book.qty_at(Side::Buy, 100) == 0);

    book.add_limit(2, Side::Buy, 100, 7);       //Same price, fresh level
    REQUIRE(book.best_bid() == 100);
    REQUIRE(book.qty_at(Side::Buy, 100) == 7);
    REQUIRE(book.order_count_at(Side::Buy, 100) == 1);
    book.check_invariants();
}

TEST_CASE("The ring holds exactly its capacity before dropping") {
    OrderBook book(Config{SelfTradePolicy::CancelResting, /*trade_capacity=*/4});
    for (lob::OrderId id = 1; id <= 4; ++id) book.add_limit(id, Side::Sell, 100, 1);

    auto rep = book.add_limit(10, Side::Buy, 100, 4);
    REQUIRE(rep.trade_count == 4);
    REQUIRE(book.trades().size() == 4);         //Exactly full
    REQUIRE(book.trades().dropped() == 0);      //Nothing lost yet

    book.add_limit(11, Side::Sell, 100, 1);
    book.add_limit(12, Side::Buy, 100, 1);      //One trade too many
    REQUIRE(book.trades().dropped() == 1);
    REQUIRE(book.trades().size() == 4);
}

TEST_CASE("Visiting resting orders yields book order") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 99, 1);
    book.add_limit(2, Side::Buy, 101, 1);       //Better bid
    book.add_limit(3, Side::Buy, 101, 1);       //Same price, later
    book.add_limit(4, Side::Sell, 105, 1);
    book.add_limit(5, Side::Sell, 103, 1);      //Better ask

    std::vector<lob::OrderId> seen;
    book.for_each_resting([&](const lob::Order& o) {seen.push_back(o.id);});

    //Bids best-first with time priority inside the level, then asks best-first.
    REQUIRE(seen == std::vector<lob::OrderId>{2, 3, 1, 5, 4});
}

TEST_CASE("Modify can lift an order through the whole opposite side") {
    OrderBook book;
    book.add_limit(1, Side::Sell, 101, 3);
    book.add_limit(2, Side::Sell, 102, 3);
    book.add_limit(3, Side::Buy, 90, 10);       //Far from the market

    auto rep = book.modify(3, 102, 10);         //Now crosses both ask levels
    REQUIRE(rep.trade_count == 2);
    REQUIRE(rep.filled == 6);
    REQUIRE(rep.remaining == 4);
    REQUIRE(rep.rested);

    REQUIRE_FALSE(book.best_ask().has_value());
    REQUIRE(book.best_bid() == 102);
    REQUIRE(book.qty_at(Side::Buy, 102) == 4);
    book.check_invariants();
}

TEST_CASE("Modify that only lowers quantity keeps the order at its price") {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);

    auto rep = book.modify(1, 100, 3);
    REQUIRE(rep.rested);
    REQUIRE(rep.trade_count == 0);
    REQUIRE(book.qty_at(Side::Buy, 100) == 3);
    REQUIRE(book.order_count_at(Side::Buy, 100) == 1);
    REQUIRE(book.size() == 1);
}

TEST_CASE("Trade sequence numbers never repeat across many orders") {
    OrderBook book;
    for (lob::OrderId id = 1; id <= 20; ++id) book.add_limit(id, Side::Sell, 100, 1);
    book.add_limit(100, Side::Buy, 100, 20);

    auto fills = drain(book);
    REQUIRE(fills.size() == 20);
    for (std::size_t i = 0; i < fills.size(); ++i)
        REQUIRE(fills[i].seq == static_cast<lob::Sequence>(i + 1));
}
