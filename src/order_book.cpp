#include "lob/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace lob {

namespace {
    std::size_t round_up_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

#if defined(__SANITIZE_ADDRESS__)
    constexpr bool kArenaAllowed = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    constexpr bool kArenaAllowed = false;
#else
    constexpr bool kArenaAllowed = true;
#endif
#else
    constexpr bool kArenaAllowed = true;
#endif

    //Arena bytes one resting order can need: a map node in case it opens a
    //level outside the price window. The order lives in the slot pool and its
    //id in the flat index, so this is the only per-order node left. Node
    //layouts are the standard library's business, so this assumes a payload
    //plus its links and rounds up to the arena's granule; provisioning only
    //has to be generous.
    constexpr std::size_t node_bytes(std::size_t payload, std::size_t links) {
        const std::size_t raw = payload + links * sizeof(void*);
        return (raw + NodeArena::kGranule - 1) / NodeArena::kGranule * NodeArena::kGranule;
    }
    constexpr std::size_t kBytesPerOrder = node_bytes(sizeof(std::pair<const Price, PriceLevel>), 4);

    constexpr std::size_t kDefaultPoolSlots = 1024;

    std::unique_ptr<NodeArena> make_arena(const Config& cfg) {
        if (!kArenaAllowed || !cfg.pool_nodes) return nullptr;
        return std::make_unique<NodeArena>(cfg.expected_orders * kBytesPerOrder);
    }

    //A window of `levels` prices centred on `price`, shifted to stay inside
    //the price range at either end.
    Price centred_base(Price price, std::size_t levels) {
        if (levels == 0) return price;
        const Price half = levels / 2;
        const Price highest = std::numeric_limits<Price>::max() - (levels - 1);
        return std::min(price >= half ? price - half : 0, highest);
    }
}

//--- PriceLadder -------------------------------------------------------------

template <bool IsBid>
PriceLadder<IsBid>::PriceLadder(std::size_t window_levels, NodeArena* arena)
    : window(window_levels), overflow(Ranking{}, ArenaAllocator<PriceLevel>(arena)) {}

template <bool IsBid>
void PriceLadder<IsBid>::place(Price base) {
    lo = base;
    //Clip so base + limit - 1 never passes the largest price.
    const Price room = std::numeric_limits<Price>::max() - base;
    limit = window.empty() || window.size() - 1 <= room ? window.size()
                                                        : static_cast<std::size_t>(room) + 1;
    is_placed = true;
}

#ifndef NDEBUG
template <bool IsBid>
void PriceLadder<IsBid>::check() const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < window.size(); ++i) {
        const PriceLevel& level = window[i];
        if (level.empty()) {
            assert(level.total_qty == 0 && "empty array slot still holds quantity");
            assert(level.tail == kNoSlot && "empty array slot still holds orders");
            continue;
        }
        assert(i < limit && "occupied slot beyond the placed window");
        ++count;
        assert(!(IsBid ? i > best_idx : i < best_idx) && "best index is not the best level");
    }
    assert(count == occupied && "occupied count out of step with the array");
    if (occupied > 0) assert(!window[best_idx].empty() && "best index points at an empty slot");
    for (const auto& [price, level] : overflow) {
        assert(!in_window(price) && "a price inside the window was filed in the map");
        assert(!level.empty() && "empty level left in the map");
    }
}
#else
template <bool IsBid>
void PriceLadder<IsBid>::check() const {}
#endif

template class PriceLadder<true>;
template class PriceLadder<false>;

//--- TradeRing ---------------------------------------------------------------

TradeRing::TradeRing(std::size_t capacity)
    : buffer(round_up_pow2(capacity == 0 ? 1 : capacity)),
      mask(buffer.size() - 1) {}

bool TradeRing::push(const Trade& t) {
    if (full()) {           //Undersized ring: count it rather than lose it quietly.
        ++drop_count;       //Callers drain between orders and watch dropped().
        return false;
    }
    buffer[head & mask] = t;
    ++head;
    return true;
}

bool TradeRing::pop(Trade& out) {
    if (empty()) return false;
    out = buffer[tail & mask];
    ++tail;
    return true;
}

//--- OrderBook ---------------------------------------------------------------

OrderBook::OrderBook(Config cfg)
    : arena(make_arena(cfg)),
      pool(cfg.expected_orders > 0 ? cfg.expected_orders : kDefaultPoolSlots),
      bids(cfg.price_levels, arena.get()),
      asks(cfg.price_levels, arena.get()),
      order_index(cfg.expected_orders > 0 ? cfg.expected_orders : kDefaultPoolSlots),
      trade_out(cfg.trade_capacity), policy(cfg.self_trade) {
    if (cfg.price_base) {
        bids.place(*cfg.price_base);
        asks.place(*cfg.price_base);
    }
}

void OrderBook::place_windows(Price price) {
    const Price base = centred_base(price, bids.capacity());
    bids.place(base);
    asks.place(base);
}

//Walk the opposite book best-price-first, filling `incoming` until it is
//exhausted or the next level no longer crosses the limit.
template <typename BookSide>
void OrderBook::match(BookSide& opposite, Order& incoming,
                      std::optional<Price> limit, ExecReport& rep) {
    const auto crosses = [&](Price level_price) {
        if (!limit) return true;    //Market order takes any price
        return incoming.side == Side::Buy ? level_price <= *limit
                                          : level_price >= *limit;
    };

    //Loop-invariant half of the self-trade test, hoisted: only an owned taker
    //under a preventing policy ever compares owners.
    const bool stp_possible = policy != SelfTradePolicy::Allow && incoming.owner != kAnonymous;

    while (incoming.qty > 0) {
        Price level_price = 0;
        PriceLevel* best = opposite.best(level_price);  //Best price on the far side
        //Most orders never cross: profiled at 95.5% of level checks.
        if (!best || !crosses(level_price)) [[likely]] break;

        PriceLevel& level = *best;
        while (incoming.qty > 0 && !level.empty()) {
            const SlotIndex slot = level.head;          //Oldest wins
            RestingOrder& resting = pool[slot].value;

            if (stp_possible && incoming.owner == resting.owner) [[unlikely]] {   //2% profiled
                if (policy == SelfTradePolicy::CancelIncoming) {
                    //Taker stops dead and the book is left untouched. Keep its
                    //unfilled quantity intact so `remaining` reports how much
                    //was cancelled; `rested` stays false, so the caller can
                    //tell this apart from a leftover that joined the book.
                    rep.stp_halted = true;
                    break;
                }
                //CancelResting: drop the maker, no trade, keep walking.
                rep.stp_cancelled += resting.qty;
                order_index.erase(resting.id);
                detach(level, slot);
                continue;
            }

            const Quantity traded = std::min(incoming.qty, resting.qty);

            incoming.qty -= traded;
            resting.qty -= traded;
            level.total_qty -= traded;

            const Trade t{next_seq, resting.id, incoming.id,
                          level_price, traded, incoming.side};
            if (rep.trade_count == 0) rep.first_seq = next_seq;
            ++next_seq;
            ++rep.trade_count;
            rep.filled += traded;
            trade_out.push(t);          //Ring, never I/O

            if (resting.qty == 0) {                     //Fully consumed
                order_index.erase(resting.id);
                detach(level, slot);
            }
        }

        if (level.empty()) [[unlikely]] opposite.closed(level_price);      //5% profiled
        if (rep.stp_halted) [[unlikely]] break;
    }
}

void OrderBook::append(PriceLevel& level, SlotIndex slot) {
    auto& s = pool[slot];
    s.prev = level.tail;
    s.next = kNoSlot;
    if (level.tail != kNoSlot) [[likely]] pool[level.tail].next = slot;  //99% profiled
    else                                   level.head = slot;
    level.tail = slot;
    level.total_qty += s.value.qty;
}

void OrderBook::detach(PriceLevel& level, SlotIndex slot) {
    const auto& s = pool[slot];
    if (s.prev != kNoSlot) pool[s.prev].next = s.next;
    else                   level.head = s.next;
    if (s.next != kNoSlot) pool[s.next].prev = s.prev;
    else                   level.tail = s.prev;
    level.total_qty -= s.value.qty;
    pool.release(slot);
}

template <typename BookSide>
void OrderBook::insert(BookSide& book, const Order& order) {
    if (!book.placed()) [[unlikely]] place_windows(order.price);    //First order to rest
    PriceLevel& level = book.open(order.price);
    const SlotIndex slot = pool.acquire();
    if (slot >= (SlotIndex{1} << 31)) [[unlikely]]
        throw std::length_error("OrderBook: over 2^31 resting orders");
    pool[slot].value = RestingOrder{order.id, order.price, order.qty, order.owner};
    append(level, slot);
    order_index.insert(order.id, index_value(slot, order.side));
}

template <typename BookSide>
void OrderBook::remove(BookSide& book, SlotIndex slot) {
    const Price price = pool[slot].value.price;
    PriceLevel* level = book.find(price);
    if (!level) [[unlikely]] return;

    detach(*level, slot);
    if (level->empty()) [[unlikely]] book.closed(price);          //0.5% profiled
}

ExecReport OrderBook::submit(OrderId id, ParticipantId owner, Side side,
                             std::optional<Price> limit, Quantity qty, bool rest_leftover) {
    ExecReport rep;
    //A zero-quantity order is a client bug, not a no-op: reject it so the
    //caller finds out, rather than silently accepting an order that can
    //never trade and never rests.
    if (qty == 0 || order_index.contains(id)) [[unlikely]] {   //Ids must be unique while live
        rep.accepted = false;
        rep.remaining = qty;
        return rep;
    }

    Order incoming{id, owner, limit.value_or(0), qty, side};

    if (side == Side::Buy) match(asks, incoming, limit, rep);
    else                   match(bids, incoming, limit, rep);

    rep.remaining = incoming.qty;
    //A taker halted by STP drops its remainder instead of resting into the
    //queue it just refused to trade with.
    if (rest_leftover && incoming.qty > 0 && !rep.stp_halted) {
        if (side == Side::Buy) insert(bids, incoming);
        else                   insert(asks, incoming);
        rep.rested = true;
    }
    check_invariants();
    return rep;
}

ExecReport OrderBook::add_limit(OrderId id, ParticipantId owner, Side side,
                                Price price, Quantity qty) {
    return submit(id, owner, side, price, qty, /*rest_leftover=*/true);
}

ExecReport OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    return submit(id, kAnonymous, side, price, qty, /*rest_leftover=*/true);
}

ExecReport OrderBook::add_market(OrderId id, ParticipantId owner, Side side, Quantity qty) {
    //No limit, and nothing rests: leftover is dropped when the book runs out.
    return submit(id, owner, side, std::nullopt, qty, /*rest_leftover=*/false);
}

ExecReport OrderBook::add_market(OrderId id, Side side, Quantity qty) {
    return submit(id, kAnonymous, side, std::nullopt, qty, /*rest_leftover=*/false);
}

bool OrderBook::cancel(OrderId id) {
    std::uint32_t entry;
    if (!order_index.take(id, entry)) [[unlikely]] return false;

    if (side_of(entry) == Side::Buy) remove(bids, slot_of(entry));
    else                             remove(asks, slot_of(entry));
    check_invariants();
    return true;
}

ExecReport OrderBook::modify(OrderId id, Price new_price, Quantity new_qty) {
    ExecReport rep;
    const std::uint32_t* entry = order_index.find(id);
    if (!entry) {                       //Nothing to modify
        rep.accepted = false;
        rep.remaining = new_qty;
        return rep;
    }

    const Side side = side_of(*entry);
    const ParticipantId owner = pool[slot_of(*entry)].value.owner;
    cancel(id);
    if (new_qty == 0) return rep;       //Modify to zero is just a cancel

    return add_limit(id, owner, side, new_price, new_qty);
}

//--- queries -----------------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const {return bids.best_price();}

std::optional<Price> OrderBook::best_ask() const {return asks.best_price();}

Volume OrderBook::qty_at(Side side, Price price) const {
    const PriceLevel* level = side == Side::Buy ? bids.find(price) : asks.find(price);
    return level ? level->total_qty : 0;
}

std::size_t OrderBook::order_count_at(Side side, Price price) const {
    const PriceLevel* level = side == Side::Buy ? bids.find(price) : asks.find(price);
    if (!level) return 0;
    std::size_t n = 0;                  //Not stored: a query, not the matching path
    for (SlotIndex i = level->head; i != kNoSlot; i = pool[i].next) ++n;
    return n;
}

std::optional<Order> OrderBook::find(OrderId id) const {
    const std::uint32_t* entry = order_index.find(id);
    if (!entry) return std::nullopt;
    const RestingOrder& r = pool[slot_of(*entry)].value;
    return Order{r.id, r.owner, r.price, r.qty, side_of(*entry)};
}

//--- invariants --------------------------------------------------------------

#ifndef NDEBUG
template <typename BookSide>
void OrderBook::check_side(const BookSide& book, Side side, std::size_t& counted) const {
    book.check();
    book.for_each_level([&](Price price, const PriceLevel& level) {
        assert(!level.empty() && "empty price level should have been erased");

        Volume sum = 0;
        std::size_t in_queue = 0;
        SlotIndex prev = kNoSlot;
        for (SlotIndex i = level.head; i != kNoSlot; i = pool[i].next) {
            const RestingOrder& o = pool[i].value;
            assert(pool[i].prev == prev && "queue's back link disagrees with its forward link");
            assert(o.price == price && "order filed under the wrong price");
            assert(o.qty > 0 && "fully filled order still resting");
            sum += o.qty;
            ++in_queue;
            prev = i;

            //The index points at this exact slot.
            const std::uint32_t* indexed = order_index.find(o.id);
            assert(indexed && "resting order missing from the index");
            assert(slot_of(*indexed) == i && "index points at a different slot");
            assert(side_of(*indexed) == side && "index files the order on the wrong side");
        }
        assert(prev == level.tail && "level's tail is not its last order");
        assert(sum == level.total_qty && "level total != sum of its orders");
        assert(level.total_qty > 0 && "level exists with zero quantity");
        counted += in_queue;
    });
}

void OrderBook::check_invariants() const {
    std::size_t counted = 0;
    check_side(bids, Side::Buy, counted);
    check_side(asks, Side::Sell, counted);
    assert(counted == order_index.size() && "index size != number of resting orders");
    assert(pool.size() == counted && "pool holds slots that no queue reaches");
    assert(pool.consistent() && "order pool free list is corrupt");
    assert(order_index.consistent() && "id index has an entry its probe cannot reach");

    const auto bid = bids.best_price();
    const auto ask = asks.best_price();
    if (bid && ask) assert(*bid < *ask && "book is crossed");
}
#else
void OrderBook::check_invariants() const {}
#endif

}
