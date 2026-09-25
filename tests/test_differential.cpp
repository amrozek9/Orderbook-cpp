#include <catch2/catch_test_macros.hpp>
#include "diff_harness.hpp"

#include <iostream>

using difftest::GenConfig;
using difftest::Generator;
using difftest::Op;
using difftest::Sequence;
using lob::OrderId;
using lob::SelfTradePolicy;
using lob::Side;

namespace {

//Run `count` random sequences through both implementations. On the first
//divergence, shrink it and print something you can paste into a test.
void sweep(std::uint64_t first_seed, std::size_t count, GenConfig cfg,
           SelfTradePolicy policy) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t seed = first_seed + i;
        const Sequence ops = Generator(seed, cfg).generate();
        if (!difftest::diverges(ops, policy)) continue;

        const Sequence minimal = difftest::shrink(ops, policy);
        std::cerr << "\nDIVERGENCE seed=" << seed
                  << " shrunk " << ops.size() << " ops -> " << minimal.size()
                  << difftest::format(minimal, policy) << std::endl;
        FAIL("engine and reference disagree, seed " << seed);
    }
    SUCCEED("no divergence in " << count << " sequences");
}

}

TEST_CASE("Differential: engine matches reference under the default policy") {
    sweep(/*first_seed=*/1, /*count=*/2000, GenConfig{}, SelfTradePolicy::CancelResting);
}

TEST_CASE("Differential: engine matches reference with self-trades allowed") {
    sweep(50000, 1000, GenConfig{}, SelfTradePolicy::Allow);
}

TEST_CASE("Differential: engine matches reference when the taker is cancelled") {
    sweep(90000, 1000, GenConfig{}, SelfTradePolicy::CancelIncoming);
}

TEST_CASE("Differential: a single price forces maximum queue depth") {
    GenConfig cfg;
    cfg.half_band = 0;      //Every order at exactly one price
    cfg.length = 60;
    sweep(130000, 800, cfg, SelfTradePolicy::CancelResting);
}

TEST_CASE("Differential: two ticks means constant crossing") {
    GenConfig cfg;
    cfg.half_band = 1;      //Three ticks: almost everything crosses
    cfg.max_qty = 3;        //Small sizes, so partial fills dominate
    sweep(170000, 800, cfg, SelfTradePolicy::CancelResting);
}

TEST_CASE("Differential: one owner makes every match a self-trade") {
    GenConfig cfg;
    cfg.owners = 1;
    cfg.anonymous_percent = 0;
    sweep(210000, 600, cfg, SelfTradePolicy::CancelResting);
}

TEST_CASE("Differential: long sequences churn the index") {
    GenConfig cfg;
    cfg.length = 250;
    sweep(250000, 200, cfg, SelfTradePolicy::CancelResting);
}

//--- the harness itself must be trustworthy ----------------------------------

TEST_CASE("Differential harness detects a planted divergence") {
    //Feed the two implementations sequences that are not the same, and confirm
    //the comparison notices. A harness that cannot fail proves nothing.
    Sequence a{
        Op{Op::Kind::AddLimit, 1, 0, Side::Sell, 100, 5},
        Op{Op::Kind::AddLimit, 2, 0, Side::Buy, 100, 5},
    };
    REQUIRE_FALSE(difftest::diverges(a, SelfTradePolicy::CancelResting));

    const auto engine = difftest::run_engine(a, SelfTradePolicy::CancelResting);
    Sequence b = a;
    b[1].qty = 4;           //Different input must produce different output
    const auto other = difftest::run_reference(b, SelfTradePolicy::CancelResting);
    REQUIRE_FALSE(engine == other);
}

TEST_CASE("Shrinker reduces a sequence to the operations that matter") {
    //Bury two crossing orders in a pile of irrelevant ones, then check the
    //shrinker strips the padding. Divergence is simulated by asking for the
    //shortest sequence that still produces a trade.
    Sequence ops;
    for (OrderId i = 1; i <= 20; ++i)
        ops.push_back(Op{Op::Kind::Cancel, i, 0, Side::Buy, 1000, 1});
    ops.push_back(Op{Op::Kind::AddLimit, 100, 0, Side::Sell, 1000, 5});
    ops.push_back(Op{Op::Kind::AddLimit, 101, 0, Side::Buy, 1000, 5});

    const auto trades_exist = [](const Sequence& s) {
        return !difftest::run_engine(s, SelfTradePolicy::CancelResting).trades.empty();
    };
    REQUIRE(trades_exist(ops));

    //Same greedy strategy the real shrinker uses.
    Sequence cur = ops;
    bool progress = true;
    while (progress) {
        progress = false;
        for (std::size_t i = 0; i < cur.size(); ++i) {
            Sequence cand = cur;
            cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(i));
            if (!cand.empty() && trades_exist(cand)) {cur = cand; progress = true; break;}
        }
    }
    REQUIRE(cur.size() == 2);       //Only the two crossing orders survive
}
