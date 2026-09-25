//-----------------------------------------------------------------------------
// Threading contract tests. These are what the ThreadSanitizer build is for.
//
// OrderBook is not thread-safe, and does not try to be. What it does promise:
//   - distinct books share no state, so they may run on separate threads
//     concurrently (one book per instrument, one thread per book);
//   - a single book may move between threads, provided the caller supplies
//     the synchronization.
// Both hold only if the engine has no hidden shared state -- a static cache, a
// global counter, a lazily initialized table. Under TSan these tests turn any
// such state into a reported race rather than an occasional wrong answer.
//-----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>
#include "diff_harness.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using difftest::GenConfig;
using difftest::Generator;
using difftest::Observed;
using difftest::Op;
using lob::SelfTradePolicy;

namespace {

constexpr std::size_t kThreads = 4;
constexpr std::size_t kSeeds = 200;

}

TEST_CASE("Independent books on separate threads match a single-threaded run") {
    //Expected results, computed with no concurrency at all.
    std::vector<Observed> expected(kSeeds);
    for (std::size_t i = 0; i < kSeeds; ++i) {
        const auto ops = Generator(i + 1, GenConfig{}).generate();
        expected[i] = difftest::run_engine(ops, SelfTradePolicy::CancelResting);
    }

    //Same work, strided across threads, each seed on its own book. Every
    //thread writes only its own slots, so the test itself is race-free and
    //anything TSan reports belongs to the engine.
    std::vector<Observed> actual(kSeeds);
    std::vector<std::thread> workers;
    for (std::size_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &actual] {
            for (std::size_t i = t; i < kSeeds; i += kThreads) {
                const auto ops = Generator(i + 1, GenConfig{}).generate();
                actual[i] = difftest::run_engine(ops, SelfTradePolicy::CancelResting);
            }
        });
    }
    for (auto& w : workers) w.join();

    for (std::size_t i = 0; i < kSeeds; ++i) {
        INFO("seed " << i + 1);
        REQUIRE(actual[i] == expected[i]);
    }
}

TEST_CASE("One book handed between threads under a lock matches a single thread") {
    const auto ops = Generator(4242, GenConfig{.length = 400}).generate();

    const auto apply = [](lob::OrderBook& book, const Op& op) {
        switch (op.kind) {
        case Op::Kind::AddLimit:  book.add_limit(op.id, op.owner, op.side, op.price, op.qty); break;
        case Op::Kind::AddMarket: book.add_market(op.id, op.owner, op.side, op.qty); break;
        case Op::Kind::Cancel:    book.cancel(op.id); break;
        case Op::Kind::Modify:    book.modify(op.id, op.price, op.qty); break;
        }
    };

    lob::OrderBook solo(lob::Config{SelfTradePolicy::CancelResting, 1u << 12});
    for (const Op& op : ops) apply(solo, op);

    //Threads take turns in a fixed order, so the operation sequence is exactly
    //the single-threaded one. The mutex is the happens-before edge that lets
    //the book change hands; remove it and TSan reports a race.
    lob::OrderBook shared(lob::Config{SelfTradePolicy::CancelResting, 1u << 12});
    std::mutex mu;
    std::condition_variable turn_changed;
    std::size_t next = 0;

    std::vector<std::thread> workers;
    for (std::size_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (;;) {
                std::unique_lock lock(mu);
                turn_changed.wait(lock, [&] {return next >= ops.size() || next % kThreads == t;});
                if (next >= ops.size()) return;
                apply(shared, ops[next]);
                ++next;
                turn_changed.notify_all();
            }
        });
    }
    for (auto& w : workers) w.join();

    shared.check_invariants();
    REQUIRE(shared.size() == solo.size());
    REQUIRE(shared.best_bid() == solo.best_bid());
    REQUIRE(shared.best_ask() == solo.best_ask());

    std::vector<lob::Trade> a, b;
    solo.drain_trades([&](const lob::Trade& t) {a.push_back(t);});
    shared.drain_trades([&](const lob::Trade& t) {b.push_back(t);});
    REQUIRE_FALSE(a.empty());
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].seq == b[i].seq);
        REQUIRE(a[i].maker_id == b[i].maker_id);
        REQUIRE(a[i].taker_id == b[i].taker_id);
        REQUIRE(a[i].price == b[i].price);
        REQUIRE(a[i].qty == b[i].qty);
    }
}
