#include "lob/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

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

    //Arena bytes one resting order can need: its list node, its hash-map node,
    //and a map node in case it opens a level. Node layouts are the standard
    //library's business, so this assumes a payload plus its links and rounds
    //up to the arena's granule; provisioning only has to be generous.
    constexpr std::size_t node_bytes(std::size_t payload, std::size_t links) {
        const std::size_t raw = payload + links * sizeof(void*);
        return (raw + NodeArena::kGranule - 1) / NodeArena::kGranule * NodeArena::kGranule;
    }
    constexpr std::size_t kBytesPerOrder =
        node_bytes(sizeof(Order), 2) +
        node_bytes(sizeof(std::pair<const OrderId, Locator>), 2) +
        node_bytes(sizeof(std::pair<const Price, PriceLevel>), 4);

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
PriceLadder<IsBid>::PriceLadder(std::size_t window_levels, NodeArena* a)
    : arena(a), overflow(Ranking{}, ArenaAllocator<PriceLevel>(a)) {
    window.reserve(window_levels);
    for (std::size_t i = 0; i < window_levels; ++i) window.emplace_back(ArenaAllocator<Order>(a));
}

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
        if (level.orders.empty()) {
            assert(level.total_qty == 0 && "empty array slot still holds quantity");
            continue;
        }
        assert(i < limit && "occupied slot beyond the placed window");
        ++count;
        assert(!(IsBid ? i > best_idx : i < best_idx) && "best index is not the best level");
    }
    assert(count == occupied && "occupied count out of step with the array");
    if (occupied > 0) assert(!window[best_idx].orders.empty() && "best index points at an empty slot");
    for (const auto& [price, level] : overflow) {
        assert(!in_window(price) && "a price inside the window was filed in the map");
        assert(!level.orders.empty() && "empty level left in the map");
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
      bids(cfg.price_levels, arena.get()),
      asks(cfg.price_levels, arena.get()),
      order_index(ArenaAllocator<Locator>(arena.get())),
      trade_out(cfg.trade_capacity), policy(cfg.self_trade) {
    if (cfg.expected_orders > 0) order_index.reserve(cfg.expected_orders);
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

    while (incoming.qty > 0) {
        Price level_price = 0;
        PriceLevel* best = opposite.best(level_price);  //Best price on the far side
        if (!best || !crosses(level_price)) break;

        PriceLevel& level = *best;
        while (incoming.qty > 0 && !level.orders.empty()) {
            Order& resting = level.orders.front();      //Oldest wins

            const bool self_trade = policy != SelfTradePolicy::Allow &&
                                    incoming.owner != kAnonymous &&
                                    incoming.owner == resting.owner;
            if (self_trade) {
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
                level.total_qty -= resting.qty;
                order_index.erase(resting.id);
                level.orders.pop_front();
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
                level.orders.pop_front();
            }
        }

        if (level.orders.empty()) opposite.closed(level_price);
        if (rep.stp_halted) break;
    }
}

template <typename BookSide>
void OrderBook::insert(BookSide& book, const Order& order) {
    if (!book.placed()) place_windows(order.price);     //First order to rest
    PriceLevel& level = book.open(order.price);
    level.total_qty += order.qty;
    level.orders.push_back(order);
    order_index[order.id] = Locator{order.price, order.side, std::prev(level.orders.end())};
}

template <typename BookSide>
void OrderBook::remove(BookSide& book, const Locator& loc) {
    PriceLevel* level = book.find(loc.price);
    if (!level) return;

    level->total_qty -= loc.order_iter->qty;
    level->orders.erase(loc.order_iter);
    if (level->orders.empty()) book.closed(loc.price);
}

ExecReport OrderBook::submit(OrderId id, ParticipantId owner, Side side,
                             std::optional<Price> limit, Quantity qty, bool rest_leftover) {
    ExecReport rep;
    //A zero-quantity order is a client bug, not a no-op: reject it so the
    //caller finds out, rather than silently accepting an order that can
    //never trade and never rests.
    if (qty == 0 || order_index.count(id)) {    //Ids must be unique while live
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
    auto it = order_index.find(id);
    if (it == order_index.end()) return false;

    const Locator loc = it->second;
    order_index.erase(it);
    if (loc.side == Side::Buy) remove(bids, loc);
    else                       remove(asks, loc);
    check_invariants();
    return true;
}

ExecReport OrderBook::modify(OrderId id, Price new_price, Quantity new_qty) {
    ExecReport rep;
    auto it = order_index.find(id);
    if (it == order_index.end()) {      //Nothing to modify
        rep.accepted = false;
        rep.remaining = new_qty;
        return rep;
    }

    const Side side = it->second.side;
    const ParticipantId owner = it->second.order_iter->owner;
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
    return level ? level->orders.size() : 0;
}

const Order* OrderBook::find(OrderId id) const {
    auto it = order_index.find(id);
    if (it == order_index.end()) return nullptr;
    return &*it->second.order_iter;
}

//--- invariants --------------------------------------------------------------

#ifndef NDEBUG
template <typename BookSide>
void OrderBook::check_side(const BookSide& book, Side side, std::size_t& counted) const {
    book.check();
    book.for_each_level([&](Price price, const PriceLevel& level) {
        assert(!level.orders.empty() && "empty price level should have been erased");

        Volume sum = 0;
        for (const Order& o : level.orders) {
            assert(o.price == price && "order filed under the wrong price");
            assert(o.side == side && "order filed on the wrong side");
            assert(o.qty > 0 && "fully filled order still resting");
            sum += o.qty;

            //The index points at this exact order.
            auto it = order_index.find(o.id);
            assert(it != order_index.end() && "resting order missing from the index");
            assert(it->second.price == price);
            assert(it->second.side == side);
            assert(&*it->second.order_iter == &o && "index iterator points elsewhere");
        }
        assert(sum == level.total_qty && "level total != sum of its orders");
        assert(level.total_qty > 0 && "level exists with zero quantity");
        counted += level.orders.size();
    });
}

void OrderBook::check_invariants() const {
    std::size_t counted = 0;
    check_side(bids, Side::Buy, counted);
    check_side(asks, Side::Sell, counted);
    assert(counted == order_index.size() && "index size != number of resting orders");

    const auto bid = bids.best_price();
    const auto ask = asks.best_price();
    if (bid && ask) assert(*bid < *ask && "book is crossed");
}
#else
void OrderBook::check_invariants() const {}
#endif

}
