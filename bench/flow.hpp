#pragma once
//-----------------------------------------------------------------------------
// Synthetic order flow for benchmarking.
//
// A benchmark only tells you about the flow it runs. Uniformly random orders
// mostly miss each other, so a naive generator ends up timing a book that does
// little but insert, and tuning for that makes the wrong path fast. This one
// follows the shape of a real equity feed:
//
//   add         50%   passive limit; most rest near the touch, a tail deeper
//   cancel      40%   mostly the orders added most recently
//   modify       5%   re-priced near the touch (cancel plus add)
//   marketable   5%   market orders and limits priced through the touch --
//                     the only operations that trade
//
// Prices are offsets from a mid that random-walks slowly, so the spread stays
// tight while depth builds up behind it.
//
// The whole sequence is generated up front into a flat array, so none of the
// generation cost lands inside a timed region. Generation drives its own copy
// of the engine to know what is live: cancels and modifies always name an
// order resting at that moment, passive adds are priced so they never cross,
// and marketable orders are sized against the depth actually there. The
// engine is deterministic, so the book the benchmark times passes through
// exactly the states the generator saw.
//
// All arithmetic is integer, so a seed yields the same flow on every platform,
// and Stats::flow_hash lets two benchmark runs confirm they timed identical
// input.
//-----------------------------------------------------------------------------
#include "lob/order_book.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace flow {

enum class Kind : std::uint8_t {Add, Cancel, Modify, Marketable};
inline constexpr std::size_t kKinds = 4;
const char* kind_name(Kind k);

struct Op {
    lob::OrderId id;
    lob::Price price;           //Unused by cancels and market orders
    lob::Quantity qty;
    lob::ParticipantId owner;
    Kind kind;
    lob::Side side;
    bool market;                //Marketable only: add_market rather than a crossing limit
};

struct Config {
    std::uint64_t seed = 1;
    std::size_t ops = 2'000'000;            //Timed operations, after the warm-up
    //Mixed operations run before recording starts, so caches, branch
    //predictors and the allocator reach steady state on the very flow that
    //is then timed. The first pass over cold state measures the wrong thing.
    std::size_t warmup_ops = 300'000;

    //Operation mix in percent; must sum to 100.
    std::uint32_t add_percent = 50;
    std::uint32_t cancel_percent = 40;
    std::uint32_t modify_percent = 5;
    std::uint32_t marketable_percent = 5;

    //Resting orders built up before the warm-up, and that marketable sizing
    //steers toward from then on.
    std::size_t target_depth = 5'000;

    //Prices, in ticks. Bids rest at mid - offset and asks at mid + 1 + offset,
    //so with both touches at offset 0 the spread is one tick.
    lob::Price start_mid = 100'000;
    std::uint32_t mid_step_per_mille = 10;  //Chance per op that mid moves a tick
    std::uint32_t near_touch_percent = 80;  //Rest geometrically close to the touch...
    std::uint32_t near_decay_percent = 50;  //...each extra tick back half as likely
    lob::Price tail_ticks = 100;            //The rest land uniformly up to this deep

    //Cancel and modify targets: usually one of the newest orders (each step
    //further back less likely), otherwise any live order.
    std::uint32_t recent_percent = 80;
    std::uint32_t recent_decay_percent = 90;

    //Order size: lot * (1 + geometric), so mostly one or two lots.
    lob::Quantity lot = 100;
    std::uint32_t size_decay_percent = 50;

    lob::ParticipantId owners = 50;
    lob::SelfTradePolicy self_trade = lob::SelfTradePolicy::CancelResting;
};

//What the generator observed while running the flow through its own book.
struct Stats {
    std::array<std::size_t, kKinds> count{};    //Timed ops of each kind
    std::size_t market_orders = 0;              //Marketable ops sent as market orders
    std::size_t exhausted = 0;                  //Marketable ops the far side could not fill
    std::size_t trades = 0;                     //Over the timed ops
    std::size_t depth_min = 0;                  //Resting orders over the timed ops
    std::size_t depth_max = 0;
    std::size_t depth_end = 0;
    lob::Price spread_p50 = 0;                  //Ticks, sampled after every timed op
    lob::Price spread_p99 = 0;
    std::uint64_t flow_hash = 0;                //Identifies the input sequence
    std::uint64_t trade_hash = 0;               //Every trade in the flow; a replay must match it
};

struct Flow {
    std::vector<Op> ops;        //Depth-building adds, then warm-up, then timed ops
    std::size_t build = 0;      //Passive adds that bring the book to target depth
    std::size_t warmup = 0;     //Mixed operations run but not recorded
    Stats stats;

    std::size_t timed_begin() const {return build + warmup;}
    std::size_t timed_count() const {return ops.size() - timed_begin();}
};

Flow generate(const Config& cfg);

//The single engine call an op maps to. Cancels report through `accepted`.
lob::ExecReport apply(lob::OrderBook& book, const Op& op);

//FNV-1a, folded field by field so struct padding never leaks in.
inline constexpr std::uint64_t kHashSeed = 0xcbf29ce484222325ull;
std::uint64_t hash_op(std::uint64_t h, const Op& op);
std::uint64_t hash_trade(std::uint64_t h, const lob::Trade& t);

//Ring size used by the generator and the benchmark. Larger than any single
//marketable order's fill count, so draining after every op never drops.
inline constexpr std::size_t kTradeCapacity = 4096;

}
