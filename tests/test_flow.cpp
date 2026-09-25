//-----------------------------------------------------------------------------
// The benchmark's numbers are only as good as the flow behind them. These pin
// down that the synthetic flow has the shape bench/flow.hpp promises: the mix,
// cancels that hit live and mostly recent orders, passive adds that never
// trade, steady depth, a tight spread, and a replay that matches generation.
//-----------------------------------------------------------------------------
#include "flow.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

namespace {

flow::Config small_config() {
    flow::Config cfg;
    cfg.ops = 20'000;
    cfg.warmup_ops = 5'000;
    cfg.target_depth = 500;
    return cfg;
}

//Generated once: debug builds check invariants after every op, twice over.
const flow::Flow& small_flow() {
    static const flow::Flow f = flow::generate(small_config());
    return f;
}

//Replays the flow into a fresh book and hands each timed op and its report to
//`visit`, draining trades as it goes.
template <typename F>
void replay(const flow::Flow& f, F&& visit) {
    lob::OrderBook book(lob::Config{small_config().self_trade, flow::kTradeCapacity});
    for (std::size_t i = 0; i < f.ops.size(); ++i) {
        const lob::ExecReport rep = flow::apply(book, f.ops[i]);
        book.drain_trades([](const lob::Trade&) {});
        if (i >= f.timed_begin()) visit(f.ops[i], rep);
    }
}

double share(std::size_t part, std::size_t whole) {
    return 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

}

TEST_CASE("flow: the same seed produces the same flow") {
    const flow::Flow again = flow::generate(small_config());
    CHECK(again.stats.flow_hash == small_flow().stats.flow_hash);
    CHECK(again.stats.trade_hash == small_flow().stats.trade_hash);
}

TEST_CASE("flow: a different seed produces a different flow") {
    flow::Config cfg = small_config();
    cfg.seed = 2;
    CHECK(flow::generate(cfg).stats.flow_hash != small_flow().stats.flow_hash);
}

TEST_CASE("flow: the realized mix matches the configured shares") {
    const flow::Config cfg = small_config();
    const auto& count = small_flow().stats.count;
    const std::size_t n = cfg.ops;
    CHECK(std::abs(share(count[0], n) - cfg.add_percent) < 1.0);
    CHECK(std::abs(share(count[1], n) - cfg.cancel_percent) < 1.0);
    CHECK(std::abs(share(count[2], n) - cfg.modify_percent) < 1.0);
    CHECK(std::abs(share(count[3], n) - cfg.marketable_percent) < 1.0);
}

TEST_CASE("flow: the build phase brings the book to its target depth") {
    const flow::Flow& f = small_flow();
    lob::OrderBook book;
    for (std::size_t i = 0; i < f.build; ++i) flow::apply(book, f.ops[i]);
    CHECK(book.size() == small_config().target_depth);
}

TEST_CASE("flow: the warm-up runs the full mix before timing starts") {
    const flow::Flow& f = small_flow();
    REQUIRE(f.warmup == small_config().warmup_ops);
    std::array<std::size_t, flow::kKinds> seen{};
    for (std::size_t i = f.build; i < f.timed_begin(); ++i)
        ++seen[static_cast<std::size_t>(f.ops[i].kind)];
    for (std::size_t k = 0; k < flow::kKinds; ++k) CHECK(seen[k] > 0);
}

TEST_CASE("flow: passive adds rest without trading") {
    std::size_t adds = 0;
    replay(small_flow(), [&](const flow::Op& op, const lob::ExecReport& rep) {
        if (op.kind != flow::Kind::Add) return;
        ++adds;
        CHECK(rep.filled == 0);
        CHECK(rep.rested);
    });
    CHECK(adds > 0);
}

TEST_CASE("flow: only marketable operations trade") {
    replay(small_flow(), [&](const flow::Op& op, const lob::ExecReport& rep) {
        if (op.kind != flow::Kind::Marketable) CHECK(rep.trade_count == 0);
    });
}

TEST_CASE("flow: marketable operations find liquidity") {
    std::size_t marketable = 0, filled = 0;
    replay(small_flow(), [&](const flow::Op& op, const lob::ExecReport& rep) {
        if (op.kind != flow::Kind::Marketable) return;
        ++marketable;
        filled += rep.filled > 0;
    });
    //Self-trade prevention can occasionally leave one with nothing to hit.
    CHECK(share(filled, marketable) > 99.0);
}

TEST_CASE("flow: cancels and modifies always name a live order") {
    replay(small_flow(), [&](const flow::Op& op, const lob::ExecReport& rep) {
        if (op.kind == flow::Kind::Cancel || op.kind == flow::Kind::Modify)
            CHECK(rep.accepted);
    });
}

TEST_CASE("flow: cancels mostly target recently added orders") {
    //Age of a cancelled order = orders added (or re-priced) since it was.
    //A marketable limit's leftover rests too, so it can be cancelled later.
    const flow::Flow& f = small_flow();
    std::unordered_map<lob::OrderId, std::size_t> added_at;
    std::size_t adds = 0;
    std::vector<std::size_t> ages;
    for (std::size_t i = 0; i < f.ops.size(); ++i) {
        const flow::Op& op = f.ops[i];
        if (op.kind == flow::Kind::Cancel && i >= f.timed_begin()) ages.push_back(adds - added_at.at(op.id));
        const bool may_rest = op.kind == flow::Kind::Add || op.kind == flow::Kind::Modify ||
                              (op.kind == flow::Kind::Marketable && !op.market);
        if (may_rest) added_at[op.id] = ++adds;
    }
    const auto recent = std::count_if(ages.begin(), ages.end(),
                                      [](std::size_t age) {return age <= 20;});
    CHECK(share(static_cast<std::size_t>(recent), ages.size()) > 50.0);
}

TEST_CASE("flow: depth holds near the target") {
    const flow::Stats& st = small_flow().stats;
    const std::size_t target = small_config().target_depth;
    CHECK(st.depth_min >= target / 2);
    CHECK(st.depth_max <= target * 2);
}

TEST_CASE("flow: the spread stays tight") {
    CHECK(small_flow().stats.spread_p50 <= 2);
}

TEST_CASE("flow: replaying reproduces the generator's trade stream") {
    const flow::Flow& f = small_flow();
    lob::OrderBook book(lob::Config{small_config().self_trade, flow::kTradeCapacity});
    std::uint64_t h = flow::kHashSeed;
    for (const flow::Op& op : f.ops) {
        flow::apply(book, op);
        book.drain_trades([&](const lob::Trade& t) {h = flow::hash_trade(h, t);});
    }
    CHECK(h == f.stats.trade_hash);
}
