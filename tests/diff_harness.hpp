#pragma once
//-----------------------------------------------------------------------------
// Randomized differential testing: drive lob::OrderBook and ref::ReferenceBook
// with the same operation sequence and require byte-identical trade streams and
// book state. On a divergence, shrink the sequence to the smallest one that
// still diverges, so what you debug is three operations rather than sixty.
//
// The generator is deliberately biased. Uniform random prices over a wide range
// almost never cross, so the matching path would barely run and the interesting
// bugs -- queue order, partial fills, level cleanup -- would never surface.
// See make_op() for the specific biases.
//-----------------------------------------------------------------------------
#include "lob/order_book.hpp"
#include "reference_book.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace difftest {

using lob::OrderId;
using lob::ParticipantId;
using lob::Price;
using lob::Quantity;
using lob::SelfTradePolicy;
using lob::Side;
using lob::Trade;

struct Op {
    enum class Kind : std::uint8_t {AddLimit, AddMarket, Cancel, Modify};
    Kind kind;
    OrderId id;
    ParticipantId owner;
    Side side;
    Price price;
    Quantity qty;
};

using Sequence = std::vector<Op>;

//Deterministic PRNG, so a failing seed reproduces exactly. Not std::mt19937,
//whose stream is fine but heavier than this needs to be.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}

    std::uint64_t next() {          //xorshift64*
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }

    //Uniform in [0, n).
    std::uint32_t below(std::uint32_t n) {
        return n == 0 ? 0 : static_cast<std::uint32_t>(next() % n);
    }

    bool chance(std::uint32_t percent) {return below(100) < percent;}

private:
    std::uint64_t state;
};

//--- generation --------------------------------------------------------------

struct GenConfig {
    //A tight band around a reference price. Every order lands within a few
    //ticks of every other, so crossing is the common case, not the rare one.
    Price mid = 1000;
    Price half_band = 3;        //Prices span [mid - 3, mid + 3]: 7 ticks
    //Only a handful of distinct prices and a small quantity range means deep
    //queues at identical prices and frequent exact/partial fills.
    Quantity max_qty = 6;
    //Few owners so self-trade prevention fires constantly.
    ParticipantId owners = 3;
    std::uint32_t anonymous_percent = 25;   //Rest use owner 1..owners
    //Cancels and modifies mostly target recently touched ids, which are the
    //ones likely to have just been filled -- that exercises cancel-of-dead-id
    //and the index cleanup path.
    std::uint32_t recent_window = 6;
    std::size_t length = 40;
};

class Generator {
public:
    Generator(std::uint64_t seed, GenConfig c) : rng(seed), cfg(c) {}

    Sequence generate() {
        Sequence ops;
        ops.reserve(cfg.length);
        for (std::size_t i = 0; i < cfg.length; ++i) ops.push_back(make_op());
        return ops;
    }

private:
    Rng rng;
    GenConfig cfg;
    OrderId next_id = 1;
    std::vector<OrderId> issued;

    //An id that was used before, weighted toward the most recent ones.
    OrderId pick_known() {
        if (issued.empty()) return 1;
        const std::uint32_t window =
            std::min<std::uint32_t>(cfg.recent_window,
                                    static_cast<std::uint32_t>(issued.size()));
        //75% recent, 25% anywhere in history (catches stale-id handling).
        const std::size_t idx = rng.chance(75)
            ? issued.size() - 1 - rng.below(window)
            : rng.below(static_cast<std::uint32_t>(issued.size()));
        return issued[idx];
    }

    Price pick_price() {
        const Price span = cfg.half_band * 2 + 1;
        return cfg.mid - cfg.half_band + rng.below(static_cast<std::uint32_t>(span));
    }

    ParticipantId pick_owner() {
        if (rng.chance(cfg.anonymous_percent)) return lob::kAnonymous;
        return 1 + rng.below(cfg.owners);
    }

    Op make_op() {
        Op op{};
        //Weighted toward adds so the book stays populated, but with enough
        //cancels and modifies to churn the index and empty levels.
        const std::uint32_t roll = rng.below(100);
        if (roll < 55)      op.kind = Op::Kind::AddLimit;
        else if (roll < 68) op.kind = Op::Kind::AddMarket;
        else if (roll < 86) op.kind = Op::Kind::Cancel;
        else                op.kind = Op::Kind::Modify;

        op.owner = pick_owner();
        op.side = rng.chance(50) ? Side::Buy : Side::Sell;
        op.price = pick_price();
        op.qty = 1 + rng.below(cfg.max_qty);

        switch (op.kind) {
        case Op::Kind::AddLimit:
        case Op::Kind::AddMarket:
            op.id = next_id++;
            issued.push_back(op.id);
            //Occasionally reuse a live id, to exercise duplicate rejection.
            if (!issued.empty() && rng.chance(4)) op.id = pick_known();
            break;
        case Op::Kind::Cancel:
        case Op::Kind::Modify:
            op.id = pick_known();
            //Occasionally target an id that was never issued at all.
            if (rng.chance(5)) op.id = next_id + 1000;
            break;
        }
        return op;
    }
};

//--- execution and comparison ------------------------------------------------

//Everything observable about a run: the trade stream, the report returned for
//each operation, and the resulting book. Comparing the reports matters because
//a report can lie while the book stays correct -- that is how a taker halted by
//self-trade prevention came to report remaining == 0.
struct Observed {
    std::vector<Trade> trades;
    std::vector<lob::ExecReport> reports;
    //Resting orders in queue order: side, price, id, qty.
    std::vector<std::array<std::uint64_t, 4>> queue;
};

inline bool same_report(const lob::ExecReport& a, const lob::ExecReport& b) {
    return a.first_seq == b.first_seq && a.trade_count == b.trade_count &&
           a.filled == b.filled && a.remaining == b.remaining &&
           a.stp_cancelled == b.stp_cancelled && a.rested == b.rested &&
           a.accepted == b.accepted && a.stp_halted == b.stp_halted;
}

inline bool operator==(const Observed& a, const Observed& b) {
    if (a.trades.size() != b.trades.size()) return false;
    for (std::size_t i = 0; i < a.trades.size(); ++i) {
        const Trade& x = a.trades[i];
        const Trade& y = b.trades[i];
        if (x.seq != y.seq || x.maker_id != y.maker_id || x.taker_id != y.taker_id ||
            x.price != y.price || x.qty != y.qty || x.taker_side != y.taker_side)
            return false;
    }
    if (a.reports.size() != b.reports.size()) return false;
    for (std::size_t i = 0; i < a.reports.size(); ++i)
        if (!same_report(a.reports[i], b.reports[i])) return false;
    return a.queue == b.queue;
}

Observed run_engine(const Sequence& ops, SelfTradePolicy policy);
Observed run_reference(const Sequence& ops, SelfTradePolicy policy);

//True when the two implementations disagree on this sequence.
inline bool diverges(const Sequence& ops, SelfTradePolicy policy) {
    return !(run_engine(ops, policy) == run_reference(ops, policy));
}

//Greedy delta-debugging: drop operations while the divergence survives.
//Shrinks a 60-op sequence to the handful that actually matter.
Sequence shrink(Sequence failing, SelfTradePolicy policy);

//Reproducible C++ for a failing sequence, ready to paste into a test.
std::string format(const Sequence& ops, SelfTradePolicy policy);

}
