//-----------------------------------------------------------------------------
// The order pool: contiguous slots addressed by 32-bit index, an intrusive free
// list, and growth that keeps every index valid. One rule per test, then the
// same rules seen through the order book.
//-----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "lob/order_book.hpp"
#include "lob/slot_pool.hpp"

#include <cstdint>
#include <set>
#include <vector>

using lob::kNoSlot;
using lob::SlotIndex;
using lob::SlotPool;

TEST_CASE("pool: hands out slots in order from a fresh block") {
    SlotPool<int> pool(4);
    CHECK(pool.acquire() == 0);
    CHECK(pool.acquire() == 1);
    CHECK(pool.acquire() == 2);
    CHECK(pool.size() == 3);
}

TEST_CASE("pool: the slot released last is the next one handed out") {
    SlotPool<int> pool(8);
    const SlotIndex a = pool.acquire();
    const SlotIndex b = pool.acquire();
    pool.acquire();
    pool.release(a);
    pool.release(b);
    CHECK(pool.acquire() == b);
    CHECK(pool.acquire() == a);
}

TEST_CASE("pool: an acquired slot starts with no links") {
    SlotPool<int> pool(4);
    const SlotIndex a = pool.acquire();
    const SlotIndex b = pool.acquire();
    pool[a].next = b;
    pool.release(a);
    const SlotIndex again = pool.acquire();
    CHECK(pool[again].prev == kNoSlot);
    CHECK(pool[again].next == kNoSlot);
}

TEST_CASE("pool: growing keeps every index and value") {
    SlotPool<std::uint64_t> pool(2);
    std::vector<SlotIndex> held;
    for (std::uint64_t v = 0; v < 100; ++v) {
        const SlotIndex i = pool.acquire();
        pool[i].value = v * 7;
        held.push_back(i);
    }
    CHECK(pool.growths() > 0);
    CHECK(pool.capacity() >= 100);
    for (std::uint64_t v = 0; v < 100; ++v) CHECK(pool[held[v]].value == v * 7);
}

TEST_CASE("pool: slots stay distinct through churn and growth") {
    SlotPool<int> pool(1);
    std::set<SlotIndex> live;
    for (int round = 0; round < 50; ++round) {
        for (int k = 0; k < 7; ++k) live.insert(pool.acquire());
        for (int k = 0; k < 4; ++k) {
            pool.release(*live.begin());
            live.erase(live.begin());
        }
        REQUIRE(pool.consistent());
    }
    CHECK(live.size() == pool.size());
    CHECK(live.size() == 150);          //Seven in, four out, fifty times
}

TEST_CASE("pool: consistent() catches a free list that loops") {
#if defined(LOB_SLOT_POOL_ASAN)
    //Corrupting the list means writing into a released slot, which is exactly
    //what the pool's poisoning makes AddressSanitizer report.
    SKIP("writes into a released slot, which ASan rightly forbids");
#endif
    SlotPool<int> pool(4);
    const SlotIndex a = pool.acquire();
    const SlotIndex b = pool.acquire();
    pool.release(a);
    pool.release(b);                    //Free list: b -> a
    REQUIRE(pool.consistent());
    pool[a].next = b;                   //Now a -> b -> a -> ...
    CHECK_FALSE(pool.consistent());
    pool[a].next = kNoSlot;             //Restore, so teardown sees a sane list
}

TEST_CASE("pool: a zero capacity still yields a usable pool") {
    SlotPool<int> pool(0);
    CHECK(pool.capacity() == 1);
    pool.acquire();
    pool.acquire();
    CHECK(pool.growths() == 1);
}

//--- through the book ----------------------------------------------------------

TEST_CASE("book: the order pool is sized by expected_orders") {
    lob::Config cfg;
    cfg.expected_orders = 5'000;
    lob::OrderBook book(cfg);
    CHECK(book.order_pool_capacity() == 5'000);
    CHECK(book.order_pool_growths() == 0);
}

TEST_CASE("book: an outgrown order pool grows and loses nothing") {
    lob::Config cfg;
    cfg.expected_orders = 2;
    lob::OrderBook book(cfg);
    for (lob::OrderId id = 1; id <= 40; ++id)
        book.add_limit(id, lob::Side::Buy, 1000 - id % 5, static_cast<lob::Quantity>(id));
    CHECK(book.order_pool_growths() > 0);
    CHECK(book.size() == 40);
    for (lob::OrderId id = 1; id <= 40; ++id) {
        const lob::Order* o = book.find(id);
        REQUIRE(o != nullptr);
        CHECK(o->qty == id);
    }
    //Time priority survives the moves: the best bid, 1000, holds ids 5, 10,
    //15, ... in arrival order, so selling 5 + 10 takes exactly the first two.
    const auto rep = book.add_market(100, lob::Side::Sell, 5 + 10);
    CHECK(rep.filled == 15);
    CHECK(book.find(5) == nullptr);
    CHECK(book.find(10) == nullptr);
    CHECK(book.find(15) != nullptr);
}

TEST_CASE("book: churn reuses freed order slots instead of growing") {
    lob::Config cfg;
    cfg.expected_orders = 8;
    lob::OrderBook book(cfg);
    for (lob::OrderId id = 1; id <= 1'000; ++id) {
        book.add_limit(id, lob::Side::Sell, 1000, 1);
        if (id > 4) book.cancel(id - 4);
    }
    CHECK(book.size() == 4);
    CHECK(book.order_pool_growths() == 0);
}
