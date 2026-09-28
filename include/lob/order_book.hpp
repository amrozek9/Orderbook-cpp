#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "lob/id_index.hpp"
#include "lob/node_arena.hpp"
#include "lob/slot_pool.hpp"

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
// caller drains. Nothing in the matching path formats or performs I/O --
// printing a fill inside the hot loop would make every measurement of it
// meaningless. Orders live in a pre-allocated slot pool, the id index is one
// flat table, and the fallback map's nodes come from a per-book NodeArena, so a
// book provisioned with Config::expected_orders does not call the system
// allocator while matching either.
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

    //An order as the API reports it. The book does not store this struct:
    //see RestingOrder.
    struct Order {
        OrderId id;
        ParticipantId owner;
        Price price;
        Quantity qty;
        Side side;
    };

    //What the book keeps per resting order: exactly what the matching loop
    //reads, and nothing else. 24 bytes, so with its two queue links a pool
    //slot is 32 -- two to a cache line, aligned so none straddles one.
    //
    //The owner stays hot because self-trade prevention compares it for every
    //resting order a taker touches. The side is the one field the loop never
    //reads: matching knows it from the ladder it walks, and cancel, modify and
    //find() reach an order through the id index, which carries the side in a
    //spare bit. No cold array is needed, because nothing is left to put in it.
    struct RestingOrder {
        OrderId id;
        Price price;
        Quantity qty;
        ParticipantId owner;
    };
    static_assert(sizeof(RestingOrder) == 24, "the hot order record grew");

    //The resting orders at one price, as a FIFO queue threaded through the
    //book's order pool by slot index: head has time priority, and each slot
    //holds its neighbours' indices. Total quantity and the two ends, nothing
    //else: 16 bytes, four to a cache line, so a sweep or a scan for the next
    //best price walks the array linearly and the prefetcher keeps up. The
    //order count is not stored; order_count_at() walks the queue instead.
    struct PriceLevel {
        Volume total_qty = 0;
        SlotIndex head = kNoSlot;   //Oldest
        SlotIndex tail = kNoSlot;   //Newest
        bool empty() const {return head == kNoSlot;}
    };
    static_assert(sizeof(PriceLevel) == 16, "a price level must stay four to a cache line");

    //One side of the book.
    //
    //Levels inside a window of consecutive prices live in a flat array indexed
    //by price - base: finding a level is one subtraction and one load, and
    //neighbouring levels share cache lines. The best level is tracked as an
    //index, so top of book is a field read. When it empties, the ladder scans
    //outward to the next occupied price, which in a live market is a tick or
    //two away.
    //
    //Prices outside the window still work: they fall back to a std::map and pay
    //its pointer chasing. The window never moves once placed, so a market that
    //drifts out of it keeps trading correctly, just slower, and
    //opened_outside() counts how often that happened. See README, "Price levels
    //live in a flat array".
    template <bool IsBid>
    class PriceLadder {
    public:
        PriceLadder(std::size_t window_levels, NodeArena* arena);

        //Fix the window at [base, base + window_levels), clipped at the top of
        //the price range. Called once, before any level opens.
        void place(Price base);
        bool placed() const {return is_placed;}
        Price base() const {return lo;}
        std::size_t span() const {return limit;}           //0 until placed
        std::size_t capacity() const {return window.size();}

        //The level at `price`, opened if nothing rests there yet.
        PriceLevel& open(Price price) {
            if (in_window(price)) {
                const std::size_t i = price - lo;
                if (window[i].empty()) note_opened(i);
                return window[i];
            }
            ++outside_opens;
            return overflow.try_emplace(price).first->second;
        }

        //The level at `price`, or nullptr when nothing rests there.
        const PriceLevel* find(Price price) const {
            if (in_window(price)) {
                const PriceLevel& level = window[price - lo];
                return level.empty() ? nullptr : &level;
            }
            const auto it = overflow.find(price);
            return it == overflow.end() ? nullptr : &it->second;
        }
        PriceLevel* find(Price price) {
            return const_cast<PriceLevel*>(std::as_const(*this).find(price));
        }

        //The best occupied level and its price, or nullptr if the side is empty.
        const PriceLevel* best(Price& price) const {
            const PriceLevel* level = nullptr;
            if (occupied > 0) {
                level = &window[best_idx];
                price = lo + best_idx;
            }
            if (!overflow.empty()) {            //Outside levels can beat the window
                const auto it = overflow.begin();
                if (!level || better(it->first, price)) {
                    price = it->first;
                    level = &it->second;
                }
            }
            return level;
        }
        PriceLevel* best(Price& price) {
            return const_cast<PriceLevel*>(std::as_const(*this).best(price));
        }

        std::optional<Price> best_price() const {
            Price price = 0;
            return best(price) ? std::optional<Price>(price) : std::nullopt;
        }

        //Call once the level at `price` has become empty.
        void closed(Price price) {
            if (!in_window(price)) {
                overflow.erase(price);
                return;
            }
            const std::size_t i = price - lo;
            if (--occupied > 0 && i == best_idx) best_idx = next_occupied(i);
        }

        std::size_t levels_outside() const {return overflow.size();}
        std::uint64_t opened_outside() const {return outside_opens;}

        //Visit every occupied level best-first, as fn(price, level): outside
        //levels better than the whole window, then the window, then the rest.
        template <typename F>
        void for_each_level(F&& fn) const {
            auto it = overflow.begin();
            for (; it != overflow.end() && beyond_best_edge(it->first); ++it)
                fn(it->first, it->second);
            if (occupied > 0) {
                if constexpr (IsBid) {
                    for (std::size_t i = best_idx + 1; i-- > 0;)
                        if (!window[i].empty()) fn(lo + i, window[i]);
                } else {
                    for (std::size_t i = best_idx; i < limit; ++i)
                        if (!window[i].empty()) fn(lo + i, window[i]);
                }
            }
            for (; it != overflow.end(); ++it) fn(it->first, it->second);
        }

        //Occupancy count, best index, and the window/overflow split. Debug only.
        void check() const;

    private:
        using Ranking = std::conditional_t<IsBid, std::greater<Price>, std::less<Price>>;
        using Overflow = std::map<Price, PriceLevel, Ranking,
                                  ArenaAllocator<std::pair<const Price, PriceLevel>>>;

        static bool better(Price a, Price b) {return IsBid ? a > b : a < b;}

        bool in_window(Price price) const {return price >= lo && price - lo < limit;}

        //Outside the window on the side that beats every price inside it.
        bool beyond_best_edge(Price price) const {
            return IsBid ? price >= lo && price - lo >= limit : price < lo;
        }

        void note_opened(std::size_t i) {
            if (occupied++ == 0 || (IsBid ? i > best_idx : i < best_idx)) best_idx = i;
        }

        //The next occupied index worse than `i`. The caller guarantees one
        //exists, so the scan needs no bounds check.
        std::size_t next_occupied(std::size_t i) const {
            if constexpr (IsBid) {
                do --i; while (window[i].empty());
            } else {
                do ++i; while (window[i].empty());
            }
            return i;
        }

        std::vector<PriceLevel> window;         //Allocated up front; empty slots are absent levels
        Overflow overflow;                      //Levels outside the window, best first
        Price lo = 0;                           //Window base
        std::size_t limit = 0;                  //Levels in use; 0 until placed
        std::size_t occupied = 0;               //Non-empty levels in the window
        std::size_t best_idx = 0;               //Valid while occupied > 0
        std::uint64_t outside_opens = 0;
        bool is_placed = false;
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
        //The fallback map's nodes come from a per-book NodeArena. False puts
        //them back on the system allocator, to benchmark the difference.
        //Always false under AddressSanitizer, so freed nodes stay poisoned and
        //quarantined.
        bool pool_nodes = true;
        //Resting orders to provision for at construction: the order pool
        //allocates and touches this many slots, the id index sizes its table
        //to stay at most half full, and the arena pre-faults room for fallback
        //levels. A book that stays within it never allocates or rehashes while
        //matching. 0 starts both at 1,024 orders, doubling as needed.
        std::size_t expected_orders = 0;
        //Consecutive prices, in ticks, whose levels live in each side's flat
        //array. Prices outside fall back to a std::map: correct, but slower.
        //Each level costs 16 bytes per side, allocated at construction. 0 puts
        //every level in the map.
        std::size_t price_levels = 1024;
        //The window's lowest price. Unset, the window is centred on the first
        //price that rests on the book.
        std::optional<Price> price_base = std::nullopt;
    };

    class OrderBook {
    public:
        explicit OrderBook(Config cfg = {});
        //Every container points into the book's own arena.
        OrderBook(const OrderBook&) = delete;
        OrderBook& operator=(const OrderBook&) = delete;

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
        //A copy of the resting order, or nothing. By value, because the book
        //keeps an order's fields in two places (its slot and its index entry).
        std::optional<Order> find(OrderId id) const;

        std::size_t size() const {return order_index.size();}
        bool empty() const {return order_index.size() == 0;}
        SelfTradePolicy self_trade_policy() const {return policy;}

        //Visit every resting order in book order: bids best-first, then asks
        //best-first, and within a price level in time priority. Useful for
        //publishing a book snapshot, and for asserting queue order in tests.
        template <typename F>
        void for_each_resting(F&& fn) const {
            const auto visit = [&](Side side) {
                return [&, side](Price, const PriceLevel& level) {
                    for (SlotIndex i = level.head; i != kNoSlot; i = pool[i].next) {
                        const RestingOrder& r = pool[i].value;
                        fn(Order{r.id, r.owner, r.price, r.qty, side});
                    }
                };
            };
            bids.for_each_level(visit(Side::Buy));
            asks.for_each_level(visit(Side::Sell));
        }

        //--- price window ----------------------------------------------
        //The flat array's lowest price, once placed.
        std::optional<Price> price_window_base() const {
            return bids.placed() ? std::optional<Price>(bids.base()) : std::nullopt;
        }
        std::size_t price_window_levels() const {return bids.span();}
        //Levels opened in the fallback map rather than the array, ever.
        std::uint64_t levels_opened_outside_window() const {
            return bids.opened_outside() + asks.opened_outside();
        }

        //Level totals equal the sum of their orders, the index size equals the
        //number of resting orders, and the book is never crossed. Asserts in
        //debug builds (and runs after every mutating call); a no-op under NDEBUG.
        void check_invariants() const;

        //True when container nodes come from the book's arena.
        bool pools_nodes() const {return arena != nullptr;}

        //--- order pool ------------------------------------------------
        std::size_t order_pool_capacity() const {return pool.capacity();}
        //Times the pool ran out and doubled, copying every order: a stall
        //that sizing Config::expected_orders correctly avoids.
        std::uint64_t order_pool_growths() const {return pool.growths();}
        //Times the id index passed half full and rehashed: the same kind of
        //stall, avoided the same way.
        std::uint64_t id_index_growths() const {return order_index.growths();}

    private:
        using OrderSlots = SlotPool<RestingOrder, 32>;
        static_assert(sizeof(OrderSlots::Slot) == 32, "an order slot must fill half a cache line");
        static_assert(alignof(OrderSlots::Slot) == 32, "an order slot must never straddle a cache line");

        //An index entry's value: the order's slot, shifted up one bit, with
        //the side in bit 0. Caps a book at 2^31 resting orders.
        static std::uint32_t index_value(SlotIndex slot, Side side) {
            return slot << 1 | (side == Side::Sell ? 1u : 0u);
        }
        static SlotIndex slot_of(std::uint32_t value) {return value >> 1;}
        static Side side_of(std::uint32_t value) {return value & 1 ? Side::Sell : Side::Buy;}

        std::unique_ptr<NodeArena> arena;       //Null: system allocator. Outlives the containers
        OrderSlots pool;                        //Every resting order, and its queue links
        PriceLadder<true> bids;
        PriceLadder<false> asks;
        IdIndex<std::uint32_t> order_index;     //OrderId -> slot and side

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
        void remove(BookSide& book, SlotIndex slot);

        //Queue maintenance. append takes a filled-in slot; detach unlinks a
        //slot, subtracts its remaining quantity from the level, and frees it.
        void append(PriceLevel& level, SlotIndex slot);
        void detach(PriceLevel& level, SlotIndex slot);

        template <typename BookSide>
        void check_side(const BookSide& book, Side side, std::size_t& counted) const;

        ExecReport submit(OrderId id, ParticipantId owner, Side side,
                          std::optional<Price> limit, Quantity qty, bool rest_leftover);

        //Places both sides' windows around `price`.
        void place_windows(Price price);
    };
}
