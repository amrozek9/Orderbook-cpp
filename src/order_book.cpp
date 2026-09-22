#include "lob/order_book.hpp"

#include <algorithm>

namespace lob {

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
            const Quantity traded = std::min(incoming.qty, resting.qty);

            incoming.qty -= traded;
            resting.qty -= traded;
            level.total_qty -= traded;

            rep.trades.push_back(Trade{resting.id, incoming.id, level_it->first, traded});
            rep.filled += traded;

            if (resting.qty == 0) {                     //Fully consumed
                order_index.erase(resting.id);
                level.orders.pop_front();
            }
        }

        if (level.orders.empty()) opposite.erase(level_it);
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

ExecReport OrderBook::submit(OrderId id, Side side,
                             std::optional<Price> limit, Quantity qty, bool rest_leftover) {
    ExecReport rep;
    if (order_index.count(id)) {        //Ids must be unique while live
        rep.accepted = false;
        rep.remaining = qty;
        return rep;
    }

    Order incoming{id, limit.value_or(0), qty, side};

    if (side == Side::Buy) match(asks, incoming, limit, rep);
    else                   match(bids, incoming, limit, rep);

    rep.remaining = incoming.qty;
    if (rest_leftover && incoming.qty > 0) {
        if (side == Side::Buy) insert(bids, incoming);
        else                   insert(asks, incoming);
        rep.rested = true;
    }
    return rep;
}

ExecReport OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    return submit(id, side, price, qty, /*rest_leftover=*/true);
}

ExecReport OrderBook::add_market(OrderId id, Side side, Quantity qty) {
    //No limit, and nothing rests: leftover is dropped when the book runs out.
    return submit(id, side, std::nullopt, qty, /*rest_leftover=*/false);
}

bool OrderBook::cancel(OrderId id) {
    auto it = order_index.find(id);
    if (it == order_index.end()) return false;

    const Locator loc = it->second;
    order_index.erase(it);
    if (loc.side == Side::Buy) remove(bids, loc);
    else                       remove(asks, loc);
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
    cancel(id);
    if (new_qty == 0) return rep;       //Modify to zero is just a cancel

    return add_limit(id, side, new_price, new_qty);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids.empty()) return std::nullopt;
    return bids.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks.empty()) return std::nullopt;
    return asks.begin()->first;
}

Quantity OrderBook::qty_at(Side side, Price price) const {
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

}
