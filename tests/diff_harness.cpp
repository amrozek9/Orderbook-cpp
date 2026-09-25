#include "diff_harness.hpp"

#include <array>
#include <sstream>

namespace difftest {

namespace {

//Queue order is the observable the two implementations must agree on: which
//orders rest, at what price, and in what priority. Both sides emit bids
//best-first then asks best-first, time priority within a level, so this
//compares priority and not just membership.
std::array<std::uint64_t, 4> row(std::uint64_t side, Price price,
                                 OrderId id, Quantity qty) {
    return {side, price, id, static_cast<std::uint64_t>(qty)};
}

}

Observed run_engine(const Sequence& ops, SelfTradePolicy policy) {
    lob::OrderBook book(lob::Config{policy, 1u << 16});
    Observed obs;

    for (const Op& op : ops) {
        switch (op.kind) {
        case Op::Kind::AddLimit:
            obs.reports.push_back(book.add_limit(op.id, op.owner, op.side, op.price, op.qty));
            break;
        case Op::Kind::AddMarket:
            obs.reports.push_back(book.add_market(op.id, op.owner, op.side, op.qty));
            break;
        case Op::Kind::Cancel: {
            lob::ExecReport r;
            r.accepted = book.cancel(op.id);     //Cancel reports only success
            obs.reports.push_back(r);
            break;
        }
        case Op::Kind::Modify:
            obs.reports.push_back(book.modify(op.id, op.price, op.qty));
            break;
        }
        book.check_invariants();    //Catch corruption at the operation that caused it
    }

    lob::Trade t;
    while (book.trades().pop(t)) obs.trades.push_back(t);
    book.for_each_resting([&](const lob::Order& o) {
        obs.queue.push_back(row(static_cast<std::uint64_t>(o.side), o.price, o.id, o.qty));
    });
    return obs;
}

Observed run_reference(const Sequence& ops, SelfTradePolicy policy) {
    ref::ReferenceBook book(policy);
    Observed obs;

    for (const Op& op : ops) {
        switch (op.kind) {
        case Op::Kind::AddLimit:
            obs.reports.push_back(book.add_limit(op.id, op.owner, op.side, op.price, op.qty));
            break;
        case Op::Kind::AddMarket:
            obs.reports.push_back(book.add_market(op.id, op.owner, op.side, op.qty));
            break;
        case Op::Kind::Cancel: {
            lob::ExecReport r;
            r.accepted = book.cancel(op.id);     //Cancel reports only success
            obs.reports.push_back(r);
            break;
        }
        case Op::Kind::Modify:
            obs.reports.push_back(book.modify(op.id, op.price, op.qty));
            break;
        }
    }

    obs.trades = book.trades();

    //queue_snapshot() already sorts bids-then-asks, best price first, arrival
    //order within a level, which is exactly the engine's traversal order.
    for (const auto& r : book.queue_snapshot())
        obs.queue.push_back(row(static_cast<std::uint64_t>(r.side), r.price, r.id, r.qty));
    return obs;
}

Sequence shrink(Sequence failing, SelfTradePolicy policy) {
    bool progress = true;
    while (progress && failing.size() > 1) {
        progress = false;
        //Try removing a chunk first, then fall back to single operations.
        for (std::size_t chunk = failing.size() / 2; chunk >= 1; chunk /= 2) {
            for (std::size_t i = 0; i + chunk <= failing.size();) {
                Sequence candidate;
                candidate.reserve(failing.size() - chunk);
                candidate.insert(candidate.end(), failing.begin(),
                                 failing.begin() + static_cast<std::ptrdiff_t>(i));
                candidate.insert(candidate.end(),
                                 failing.begin() + static_cast<std::ptrdiff_t>(i + chunk),
                                 failing.end());
                if (!candidate.empty() && diverges(candidate, policy)) {
                    failing = std::move(candidate);
                    progress = true;
                } else {
                    ++i;
                }
            }
            if (chunk == 1) break;
        }
    }
    return failing;
}

std::string format(const Sequence& ops, SelfTradePolicy policy) {
    const char* pol = policy == SelfTradePolicy::Allow          ? "Allow"
                    : policy == SelfTradePolicy::CancelIncoming ? "CancelIncoming"
                                                                : "CancelResting";
    std::ostringstream o;
    o << "\n// SelfTradePolicy::" << pol << ", " << ops.size() << " ops\n";
    o << "lob::OrderBook book(lob::Config{lob::SelfTradePolicy::" << pol << ", 4096});\n";
    for (const Op& op : ops) {
        const char* side = op.side == Side::Buy ? "Side::Buy" : "Side::Sell";
        switch (op.kind) {
        case Op::Kind::AddLimit:
            o << "book.add_limit(" << op.id << ", " << op.owner << ", " << side
              << ", " << op.price << ", " << op.qty << ");\n";
            break;
        case Op::Kind::AddMarket:
            o << "book.add_market(" << op.id << ", " << op.owner << ", " << side
              << ", " << op.qty << ");\n";
            break;
        case Op::Kind::Cancel:
            o << "book.cancel(" << op.id << ");\n";
            break;
        case Op::Kind::Modify:
            o << "book.modify(" << op.id << ", " << op.price << ", " << op.qty << ");\n";
            break;
        }
    }
    return o.str();
}

}
