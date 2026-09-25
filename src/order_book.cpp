#include "lob/order_book.hpp"

#include <algorithm>
#include <cassert>

namespace lob {

namespace {
    std::size_t round_up_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }
}

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
    : trade_out(cfg.trade_capacity), policy(cfg.self_trade) {}

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

    while (incoming.qty > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();       //Best price on the far side
        if (!crosses(level_it->first)) break;

        PriceLevel& level = level_it->second;
        while (incoming.qty > 0 && !level.orders.empty()) {
            Order& resting = level.orders.front();      //Oldest wins

            const bool self_trade = policy != SelfTradePolicy::Allow &&
                                    incoming.owner != kAnonymous &&
                                    incoming.owner == resting.owner;
            if (self_trade) {
                if (policy == SelfTradePolicy::CancelIncoming) {
                    rep.stp_halted = true;      //Taker stops dead; book untouched
                    incoming.qty = 0;
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
                          level_it->first, traded, incoming.side};
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

        if (level.orders.empty()) opposite.erase(level_it);
        if (rep.stp_halted) break;
    }
}

template <typename BookSide>
void OrderBook::insert(BookSide& book, const Order& order) {
    PriceLevel& level = book[order.price];
    level.total_qty += order.qty;
    level.orders.push_back(order);
    order_index[order.id] = Locator{order.price, order.side, std::prev(level.orders.end())};
}

template <typename BookSide>
void OrderBook::remove(BookSide& book, const Locator& loc) {
    auto level_it = book.find(loc.price);
    if (level_it == book.end()) return;

    PriceLevel& level = level_it->second;
    level.total_qty -= loc.order_iter->qty;
    level.orders.erase(loc.order_iter);
    if (level.orders.empty()) book.erase(level_it);
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

std::optional<Price> OrderBook::best_bid() const {
    if (bids.empty()) return std::nullopt;
    return bids.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks.empty()) return std::nullopt;
    return asks.begin()->first;
}

Volume OrderBook::qty_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids.find(price);
        return it == bids.end() ? 0 : it->second.total_qty;
    }
    auto it = asks.find(price);
    return it == asks.end() ? 0 : it->second.total_qty;
}

std::size_t OrderBook::order_count_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids.find(price);
        return it == bids.end() ? 0 : it->second.orders.size();
    }
    auto it = asks.find(price);
    return it == asks.end() ? 0 : it->second.orders.size();
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
    for (const auto& [price, level] : book) {
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
    }
}

void OrderBook::check_invariants() const {
    std::size_t counted = 0;
    check_side(bids, Side::Buy, counted);
    check_side(asks, Side::Sell, counted);
    assert(counted == order_index.size() && "index size != number of resting orders");

    if (!bids.empty() && !asks.empty()) {
        assert(bids.begin()->first < asks.begin()->first && "book is crossed");
    }
}
#else
void OrderBook::check_invariants() const {}
#endif

}
