#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

//-----------------------------------------------------------------------------
// Determinism contract
//
// The engine is a pure function of its input sequence. It reads no clock, draws
// no randomness, and touches no global state; time priority comes from arrival
// order, not wall time. The same orders in produce the same trades out, byte for
// byte, on every run and every machine. That is what makes replay testing and
// A/B benchmarking possible, so do not introduce any of the above.
//
// Trades leave through a fixed-capacity ring buffer (lob::TradeRing) that the
// caller drains. Nothing in the matching path allocates, formats, or performs
// I/O -- printing a fill inside the hot loop would make every measurement of it
// meaningless.
//
// Threading: an OrderBook is not thread-safe. Distinct books share no state and
// may run concurrently on separate threads; a single book may move between
// threads only with caller-supplied synchronization. tests/test_threading.cpp
// checks both under ThreadSanitizer.
//-----------------------------------------------------------------------------

namespace lob {
    using OrderId = std::uint64_t;
    using Price = std::uint64_t;    //Ticks
    using Quantity = std::uint32_t;
    //Aggregates (level totals, resting volume) need more room than one
    //order's quantity: a level can hold many orders of near-max size.
    using Volume = std::uint64_t;
    using ParticipantId = std::uint32_t;
    using Sequence = std::uint64_t;

    //Orders owned by kAnonymous never trigger self-trade prevention.
    inline constexpr ParticipantId kAnonymous = 0;

    enum class Side : std::uint8_t {Buy, Sell};

    //What happens when an order would match another order with the same owner.
    //See README.md for the rationale behind the default.
    enum class SelfTradePolicy : std::uint8_t {
        Allow,              //Print the wash trade and carry on
        CancelResting,      //Kill the resting order, no trade, taker keeps walking
        CancelIncoming      //Stop the taker dead, drop its remainder, book untouched
    };

    struct Order {
        OrderId id;
        ParticipantId owner;
        Price price;
        Quantity qty;
        Side side;
    };

    struct PriceLevel {
        Volume total_qty = 0;
        std::list<Order> orders;    //FIFO: front has time priority
    };

    struct Locator {
        Price price;
        Side side;
        std::list<Order>::iterator order_iter;
    };

    //One fill. Always printed at the resting (maker) order's price.
    //`seq` is a monotonic counter over the book's whole trade stream, so two
    //replays of one input sequence produce identical seq numbers.
    struct Trade {
        Sequence seq;
        OrderId maker_id;
        OrderId taker_id;
        Price price;
        Quantity qty;
        Side taker_side;
    };

    //Fixed-capacity single-threaded ring of trades. Allocates once, at
    //construction; push/pop are O(1) and allocation-free.
    class TradeRing {
    public:
        explicit TradeRing(std::size_t capacity = 4096);

        //False when full: the trade is dropped and counted, never silently lost.
        bool push(const Trade& t);
        bool pop(Trade& out);

        std::size_t size() const {return head - tail;}
        std::size_t capacity() const {return buffer.size();}
        bool empty() const {return head == tail;}
        bool full() const {return size() == buffer.size();}
        std::uint64_t dropped() const {return drop_count;}

    private:
        std::vector<Trade> buffer;      //Size is a power of two
        std::size_t mask = 0;
        std::size_t head = 0;           //Write cursor
        std::size_t tail = 0;           //Read cursor
        std::uint64_t drop_count = 0;
    };

    //What happened to an incoming order. Holds no container, so submitting an
    //order allocates nothing; the fills themselves went to the ring, and
    //[first_seq, first_seq + trade_count) identifies them.
    struct ExecReport {
        Sequence first_seq = 0;             //Seq of this order's first trade, 0 if none
        std::uint32_t trade_count = 0;
        Quantity filled = 0;                //Total matched
        Quantity remaining = 0;             //Unmatched leftover
        Volume stp_cancelled = 0;           //Resting qty killed by self-trade prevention
        bool rested = false;                //Leftover joined the book (limit only)
        bool accepted = true;               //False if the id was already live
        bool stp_halted = false;            //Cut short by SelfTradePolicy::CancelIncoming
    };

    struct Config {
        SelfTradePolicy self_trade = SelfTradePolicy::CancelResting;
        std::size_t trade_capacity = 4096;
    };

    class OrderBook {
    public:
        explicit OrderBook(Config cfg = {});

        //Match against the far side, then rest any leftover at `price`.
        ExecReport add_limit(OrderId id, ParticipantId owner, Side side, Price price, Quantity qty);
        ExecReport add_limit(OrderId id, Side side, Price price, Quantity qty);

        //Match against the far side at any price; leftover is discarded
        //(remaining > 0 means the book ran out of liquidity).
        ExecReport add_market(OrderId id, ParticipantId owner, Side side, Quantity qty);
        ExecReport add_market(OrderId id, Side side, Quantity qty);

        //Remove a resting order. False if the id is not live.
        bool cancel(OrderId id);

        //Cancel plus add: the order keeps its id and owner but loses time
        //priority, and may cross if the new price is aggressive.
        ExecReport modify(OrderId id, Price new_price, Quantity new_qty);

        //--- trade output ----------------------------------------------
        TradeRing& trades() {return trade_out;}
        const TradeRing& trades() const {return trade_out;}

        //Convenience for non-benchmark callers: drain the ring into a callback.
        //Call it between orders, never from inside the matching loop.
        template <typename F>
        std::size_t drain_trades(F&& on_trade) {
            std::size_t n = 0;
            Trade t;
            while (trade_out.pop(t)) {on_trade(t); ++n;}
            return n;
        }

        //--- top of book / queries -------------------------------------
        std::optional<Price> best_bid() const;
        std::optional<Price> best_ask() const;
        Volume qty_at(Side side, Price price) const;
        std::size_t order_count_at(Side side, Price price) const;
        const Order* find(OrderId id) const;

        std::size_t size() const {return order_index.size();}
        bool empty() const {return order_index.empty();}
        SelfTradePolicy self_trade_policy() const {return policy;}

        //Visit every resting order in book order: bids best-first, then asks
        //best-first, and within a price level in time priority. Useful for
        //publishing a book snapshot, and for asserting queue order in tests.
        template <typename F>
        void for_each_resting(F&& fn) const {
            for (const auto& [price, level] : bids)
                for (const Order& o : level.orders) fn(o);
            for (const auto& [price, level] : asks)
                for (const Order& o : level.orders) fn(o);
        }

        //Level totals equal the sum of their orders, the index size equals the
        //number of resting orders, and the book is never crossed. Asserts in
        //debug builds (and runs after every mutating call); a no-op under NDEBUG.
        void check_invariants() const;

    private:
        std::map<Price, PriceLevel, std::greater<Price>> bids; //Best bid first
        std::map<Price, PriceLevel, std::less<Price>> asks;    //Best ask first
        std::unordered_map<OrderId, Locator> order_index;      //OrderId -> where it rests

        TradeRing trade_out;
        SelfTradePolicy policy;
        Sequence next_seq = 1;          //0 means "no trade" in ExecReport

        //`limit` empty means "any price" (market order).
        template <typename BookSide>
        void match(BookSide& opposite, Order& incoming,
                   std::optional<Price> limit, ExecReport& rep);

        template <typename BookSide>
        void insert(BookSide& book, const Order& order);

        template <typename BookSide>
        void remove(BookSide& book, const Locator& loc);

        template <typename BookSide>
        void check_side(const BookSide& book, Side side, std::size_t& counted) const;

        ExecReport submit(OrderId id, ParticipantId owner, Side side,
                          std::optional<Price> limit, Quantity qty, bool rest_leftover);
    };
}
