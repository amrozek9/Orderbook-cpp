//-----------------------------------------------------------------------------
// The node arena, and the claim it exists for: a provisioned book's matching
// path never calls the system allocator.
//
// That claim is checked directly, by counting every call to the global
// operator new. The replacement below is program-wide, so it counts for the
// whole test binary; it only counts, and hands every request to malloc.
//-----------------------------------------------------------------------------
#include "lob/node_arena.hpp"
#include "lob/order_book.hpp"
#include "flow.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <set>
#include <vector>

namespace {
    std::atomic<std::size_t> g_system_allocations{0};
}

void* operator new(std::size_t n) {
    g_system_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p, std::size_t) noexcept {std::free(p);}

namespace {

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAsan = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kAsan = true;
#else
constexpr bool kAsan = false;
#endif
#else
constexpr bool kAsan = false;
#endif

//Runs a small benchmark flow through `book` and returns how many times the
//system allocator was called while it did.
std::size_t allocations_while_matching(lob::OrderBook& book) {
    flow::Config cfg;
    cfg.ops = 20'000;
    cfg.warmup_ops = 5'000;
    cfg.target_depth = 500;
    const flow::Flow f = flow::generate(cfg);

    const std::size_t before = g_system_allocations.load();
    for (const flow::Op& op : f.ops) {
        flow::apply(book, op);
        book.drain_trades([](const lob::Trade&) {});
    }
    return g_system_allocations.load() - before;
}

}

TEST_CASE("arena: a freed block is the next one handed out for its size") {
    lob::NodeArena arena;
    void* a = arena.allocate(48);
    arena.allocate(48);
    arena.deallocate(a, 48);
    CHECK(arena.allocate(48) == a);
}

TEST_CASE("arena: size classes never share a freed block") {
    lob::NodeArena arena;
    void* small = arena.allocate(32);
    arena.deallocate(small, 32);
    CHECK(arena.allocate(80) != small);
}

TEST_CASE("arena: blocks are aligned to its granule") {
    lob::NodeArena arena;
    for (std::size_t bytes : {1u, 8u, 24u, 40u, 48u, 80u, 200u, 256u}) {
        const auto addr = reinterpret_cast<std::uintptr_t>(arena.allocate(bytes));
        CHECK(addr % lob::NodeArena::kGranule == 0);
    }
}

TEST_CASE("arena: live blocks never overlap, across slab boundaries") {
    lob::NodeArena arena;
    std::set<std::uintptr_t> starts;
    const std::size_t count = 3 * lob::NodeArena::kSlabBytes / 256;
    for (std::size_t i = 0; i < count; ++i) {
        auto* p = static_cast<unsigned char*>(arena.allocate(256));
        p[0] = p[255] = 0xAB;                   //Writable end to end
        starts.insert(reinterpret_cast<std::uintptr_t>(p));
    }
    REQUIRE(starts.size() == count);
    std::uintptr_t prev = 0;
    for (std::uintptr_t s : starts) {
        CHECK((prev == 0 || s - prev >= 256));
        prev = s;
    }
    CHECK(arena.slab_bytes() >= 3 * lob::NodeArena::kSlabBytes);
}

TEST_CASE("arena: requests larger than a block bypass the slabs") {
    lob::NodeArena arena;
    void* p = arena.allocate(lob::NodeArena::kMaxBlock + 1);
    CHECK(arena.slab_bytes() == 0);
    arena.deallocate(p, lob::NodeArena::kMaxBlock + 1);
}

TEST_CASE("arena: provisioning allocates slab space up front") {
    lob::NodeArena arena(1 << 20);
    CHECK(arena.slab_bytes() >= (1u << 20));
}

TEST_CASE("book: pools its nodes by default, except under AddressSanitizer") {
    lob::OrderBook book;
    CHECK(book.pools_nodes() == !kAsan);
}

TEST_CASE("book: pool_nodes = false puts nodes on the system allocator") {
    lob::Config cfg;
    cfg.pool_nodes = false;
    lob::OrderBook book(cfg);
    CHECK_FALSE(book.pools_nodes());
}

TEST_CASE("book: a provisioned book never calls the system allocator while matching") {
    if (kAsan) SKIP("the arena is bypassed under AddressSanitizer");
    lob::Config cfg;
    cfg.expected_orders = 2'000;        //The flow peaks well below this
    lob::OrderBook book(cfg);
    CHECK(allocations_while_matching(book) == 0);
}

TEST_CASE("book: the same flow on the system allocator does allocate") {
    //Keeps the test above honest: the counter sees container allocations.
    lob::Config cfg;
    cfg.pool_nodes = false;
    lob::OrderBook book(cfg);
    CHECK(allocations_while_matching(book) > 10'000);
}
