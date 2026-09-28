//-----------------------------------------------------------------------------
// The open-addressed id index. The rules first, one per test; then a long
// randomized churn against std::unordered_map, checking after every operation
// that each entry can still be reached from its home slot -- the property
// backward-shift deletion exists to keep.
//-----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "lob/id_index.hpp"

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

using Index = lob::IdIndex<std::uint32_t>;

TEST_CASE("id index: finds what was inserted, and nothing else") {
    Index index(8);
    index.insert(7, 70);
    index.insert(8, 80);
    REQUIRE(index.find(7));
    CHECK(*index.find(7) == 70);
    CHECK(*index.find(8) == 80);
    CHECK_FALSE(index.find(9));
    CHECK(index.size() == 2);
}

TEST_CASE("id index: take returns the value and removes the id") {
    Index index(8);
    index.insert(5, 50);
    std::uint32_t out = 0;
    CHECK(index.take(5, out));
    CHECK(out == 50);
    CHECK_FALSE(index.contains(5));
    CHECK_FALSE(index.take(5, out));
    CHECK(index.size() == 0);
}

TEST_CASE("id index: the extreme ids are ordinary keys") {
    Index index(8);
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    index.insert(0, 1);
    index.insert(max, 2);
    CHECK(*index.find(0) == 1);
    CHECK(*index.find(max) == 2);
}

TEST_CASE("id index: stays at most half full, growing when it must") {
    Index index(8);
    const std::size_t start = index.capacity();
    for (std::uint64_t id = 1; id <= start / 2; ++id) index.insert(id, 0);
    CHECK(index.growths() == 0);
    index.insert(start, 0);
    CHECK(index.growths() == 1);
    CHECK(index.capacity() == start * 2);
    for (std::uint64_t id = 1; id <= start / 2; ++id) CHECK(index.contains(id));
}

TEST_CASE("id index: provisioned for n ids, holds n without growing") {
    Index index(10'000);
    for (std::uint64_t id = 1; id <= 10'000; ++id) index.insert(id, 0);
    CHECK(index.growths() == 0);
    CHECK(index.consistent());
}

TEST_CASE("id index: strided ids spread instead of colliding") {
    //Multiples of the capacity would all land on one slot under an identity
    //hash. Folding in the high bits must still find every one quickly.
    Index index(1'024);
    for (std::uint64_t k = 1; k <= 500; ++k) index.insert(k * 4096, static_cast<std::uint32_t>(k));
    for (std::uint64_t k = 1; k <= 500; ++k) {
        REQUIRE(index.find(k * 4096));
        CHECK(*index.find(k * 4096) == k);
    }
    CHECK(index.consistent());
}

TEST_CASE("id index: matches std::unordered_map through heavy churn") {
    //Small table, many collisions, many deletions: every erase shifts entries
    //back, and any mistake leaves one unreachable.
    Index index(4);
    std::unordered_map<std::uint64_t, std::uint32_t> model;
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto next = [&] {
        state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    };
    for (int op = 0; op < 60'000; ++op) {
        const std::uint64_t id = next() % 200;          //Small key space: constant reuse
        const auto value = static_cast<std::uint32_t>(next());
        if (next() % 2) {
            if (!model.count(id)) {
                index.insert(id, value);
                model[id] = value;
            }
        } else {
            std::uint32_t out = 0;
            const bool had = model.erase(id) == 1;
            REQUIRE(index.take(id, out) == had);
        }
        REQUIRE(index.size() == model.size());
        REQUIRE(index.consistent());
    }
    for (const auto& [id, value] : model) {
        REQUIRE(index.find(id));
        CHECK(*index.find(id) == value);
    }
}
