//-----------------------------------------------------------------------------
// Standalone differential fuzzer, for running far more sequences than belong in
// a unit-test run. The Catch2 suite covers a few thousand on every build; this
// is what you point at millions overnight or in CI.
//
//   ./difftest [sequences] [first_seed] [ops_per_sequence]
//
// Exits non-zero on the first divergence, after shrinking it and printing a
// reproducer. Deterministic: the same seed always produces the same sequence.
//-----------------------------------------------------------------------------
#include "diff_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <string>

int main(int argc, char** argv) {
    const std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    const std::uint64_t first_seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    const std::size_t length = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 40;

    const lob::SelfTradePolicy policies[] = {
        lob::SelfTradePolicy::CancelResting,
        lob::SelfTradePolicy::Allow,
        lob::SelfTradePolicy::CancelIncoming,
    };

    //Price-window shapes against the generator's 997..1003 band: the default
    //(everything in the array), a window narrower than the band, one level
    //centred on the first resting price, a small centred window, and none.
    struct Window {
        std::size_t levels;
        std::optional<lob::Price> base;
    };
    const Window windows[] = {
        {lob::Config{}.price_levels, std::nullopt},
        {3, 999},
        {1, std::nullopt},
        {4, std::nullopt},
        {0, std::nullopt},
    };

    difftest::GenConfig cfg;
    cfg.length = length;

    std::printf("running %zu sequences x %zu ops, seeds %llu..%llu\n",
                count, length, (unsigned long long)first_seed,
                (unsigned long long)(first_seed + count - 1));

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t seed = first_seed + i;
        //Rotate the policy and the price window's shape, so every policy meets
        //every way a level can be filed: in the array, in the fallback map,
        //and crossing between them.
        lob::Config engine = difftest::engine_config(policies[seed % 3]);
        const Window& w = windows[(seed / 3) % std::size(windows)];
        engine.price_levels = w.levels;
        engine.price_base = w.base;
        const auto ops = difftest::Generator(seed, cfg).generate();

        if (difftest::diverges(ops, engine)) {
            const auto minimal = difftest::shrink(ops, engine);
            std::printf("\nDIVERGENCE at seed %llu: shrunk %zu ops -> %zu\n%s\n",
                        (unsigned long long)seed, ops.size(), minimal.size(),
                        difftest::format(minimal, engine).c_str());
            return 1;
        }

        if ((i + 1) % 25000 == 0)
            std::printf("  %zu/%zu clean\n", i + 1, count);
    }

    std::printf("all %zu sequences agree\n", count);
    return 0;
}
