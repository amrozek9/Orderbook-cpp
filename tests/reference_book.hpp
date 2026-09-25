#pragma once
//-----------------------------------------------------------------------------
// A deliberately naive order book, for differential testing only.
//
// This exists to disagree with lob::OrderBook. It is written to be obviously
// correct rather than fast: every resting order lives in one flat vector, and
// every operation is a linear scan. There is no price map, no per-level list,
// and no id index, so it shares no data structure and no bug with the real
// engine. If the two ever produce different trades, one of them is wrong.
//
// Semantics it mirrors exactly, because the real book defines them:
//   - trades print at the resting order's price
//   - price priority first, then arrival order
//   - modify is cancel plus add: same id and owner, new queue position
//   - market orders never rest; limit leftovers do
//   - zero-quantity and duplicate-live-id orders are rejected
//   - self-trade prevention per lob::SelfTradePolicy
//-----------------------------------------------------------------------------
#include "lob/order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace ref {

using lob::OrderId;
using lob::ParticipantId;
using lob::Price;
using lob::Quantity;
using lob::Sequence;
using lob::SelfTradePolicy;
using lob::Side;
using lob::Trade;
using lob::Volume;

class ReferenceBook {
public:
    struct Resting {
        OrderId id;
        ParticipantId owner;
        Price price;
        Quantity qty;
        Side side;
        std::uint64_t arrival;      //Lower arrives earlier
    };

    explicit ReferenceBook(SelfTradePolicy stp = SelfTradePolicy::CancelResting)
        : policy(stp) {}

    lob::ExecReport add_limit(OrderId id, ParticipantId owner, Side side,
                              Price price, Quantity qty) {
        return submit(id, owner, side, price, qty, /*is_market=*/false);
    }

    lob::ExecReport add_market(OrderId id, ParticipantId owner, Side side, Quantity qty) {
        return submit(id, owner, side, 0, qty, /*is_market=*/true);
    }

    bool cancel(OrderId id) {
        for (std::size_t i = 0; i < resting.size(); ++i) {
            if (resting[i].id == id) {
                resting.erase(resting.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

    lob::ExecReport modify(OrderId id, Price new_price, Quantity new_qty) {
        lob::ExecReport rep;
        const Resting* live = find(id);
        if (!live) {
            rep.accepted = false;
            rep.remaining = new_qty;
            return rep;
        }
        const Side side = live->side;
        const ParticipantId owner = live->owner;
        cancel(id);
        if (new_qty == 0) return rep;       //Modify to zero is a cancel
        return add_limit(id, owner, side, new_price, new_qty);
    }

    //--- queries, all linear ------------------------------------------------
    std::optional<Price> best_bid() const {return best_price(Side::Buy);}
    std::optional<Price> best_ask() const {return best_price(Side::Sell);}

    Volume qty_at(Side side, Price price) const {
        Volume total = 0;
        for (const Resting& r : resting)
            if (r.side == side && r.price == price) total += r.qty;
        return total;
    }

    std::size_t size() const {return resting.size();}
    bool empty() const {return resting.empty();}

    const std::vector<Trade>& trades() const {return trade_log;}
    void clear_trades() {trade_log.clear();}

    //Resting orders in queue order per side: price priority, then arrival.
    std::vector<Resting> queue_snapshot() const {
        std::vector<Resting> out = resting;
        std::sort(out.begin(), out.end(), [](const Resting& a, const Resting& b) {
            if (a.side != b.side) return a.side < b.side;
            if (a.price != b.price)
                return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
            return a.arrival < b.arrival;
        });
        return out;
    }

private:
    std::vector<Resting> resting;
    std::vector<Trade> trade_log;
    SelfTradePolicy policy;
    std::uint64_t next_arrival = 1;
    Sequence next_seq = 1;

    const Resting* find(OrderId id) const {
        for (const Resting& r : resting)
            if (r.id == id) return &r;
        return nullptr;
    }

    std::optional<Price> best_price(Side side) const {
        std::optional<Price> best;
        for (const Resting& r : resting) {
            if (r.side != side) continue;
            if (!best) {best = r.price; continue;}
            if (side == Side::Buy ? r.price > *best : r.price < *best) best = r.price;
        }
        return best;
    }

    //Index of the order a taker on `side` should hit next, ignoring price
    //limits. Best price wins; ties break on arrival.
    std::optional<std::size_t> next_maker(Side taker_side) const {
        const Side maker_side = taker_side == Side::Buy ? Side::Sell : Side::Buy;
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < resting.size(); ++i) {
            const Resting& r = resting[i];
            if (r.side != maker_side) continue;
            if (!best) {best = i; continue;}
            const Resting& b = resting[*best];
            const bool better_price = maker_side == Side::Sell ? r.price < b.price
                                                               : r.price > b.price;
            const bool same_price = r.price == b.price;
            if (better_price || (same_price && r.arrival < b.arrival)) best = i;
        }
        return best;
    }

    lob::ExecReport submit(OrderId id, ParticipantId owner, Side side,
                           Price price, Quantity qty, bool is_market) {
        lob::ExecReport rep;
        if (qty == 0 || find(id) != nullptr) {
            rep.accepted = false;
            rep.remaining = qty;
            return rep;
        }

        Quantity left = qty;
        while (left > 0) {
            const auto maker_idx = next_maker(side);
            if (!maker_idx) break;                      //Book ran out
            Resting& maker = resting[*maker_idx];

            if (!is_market) {                           //Limit price gate
                const bool crosses = side == Side::Buy ? maker.price <= price
                                                       : maker.price >= price;
                if (!crosses) break;
            }

            const bool self = policy != SelfTradePolicy::Allow &&
                              owner != lob::kAnonymous && owner == maker.owner;
            if (self) {
                if (policy == SelfTradePolicy::CancelIncoming) {
                    //Keep `left` intact: the unfilled quantity is cancelled,
                    //and `remaining` must say so rather than reading as a fill.
                    rep.stp_halted = true;
                    break;
                }
                rep.stp_cancelled += maker.qty;         //CancelResting
                resting.erase(resting.begin() + static_cast<std::ptrdiff_t>(*maker_idx));
                continue;
            }

            const Quantity traded = std::min(left, maker.qty);
            left -= traded;
            maker.qty -= traded;

            Trade t{};
            t.seq = next_seq;
            t.maker_id = maker.id;
            t.taker_id = id;
            t.price = maker.price;
            t.qty = traded;
            t.taker_side = side;
            if (rep.trade_count == 0) rep.first_seq = next_seq;
            ++next_seq;
            ++rep.trade_count;
            rep.filled += traded;
            trade_log.push_back(t);

            if (maker.qty == 0)
                resting.erase(resting.begin() + static_cast<std::ptrdiff_t>(*maker_idx));
        }

        rep.remaining = left;
        if (!is_market && left > 0 && !rep.stp_halted) {
            resting.push_back(Resting{id, owner, price, left, side, next_arrival++});
            rep.rested = true;
        }
        return rep;
    }
};

}
