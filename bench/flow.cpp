#include "flow.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace flow {

namespace {

//xorshift64*: deterministic and cheap. Generation is untimed, but a flow of
//tens of millions of ops should still take seconds, not minutes.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}

    std::uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }

    //Uniform in [0, n).
    std::uint64_t below(std::uint64_t n) {return n == 0 ? 0 : next() % n;}
    bool chance(std::uint32_t percent) {return below(100) < percent;}
    bool chance_per_mille(std::uint32_t per_mille) {return below(1000) < per_mille;}

    //Consecutive successes, capped: mean ~ percent / (100 - percent).
    std::uint32_t geometric(std::uint32_t percent, std::uint32_t cap) {
        std::uint32_t n = 0;
        while (n < cap && chance(percent)) ++n;
        return n;
    }

private:
    std::uint64_t state;
};

//Live order ids, oldest first. Orders that fill leave stale ids behind; they
//are skipped when drawn and swept out periodically.
class LivePool {
public:
    void push(lob::OrderId id) {ids.push_back(id);}

    //Remove and return a live id, usually one of the newest. The caller
    //pushes it back if the order survives (a modify re-prices it, which
    //makes it recent again).
    std::optional<lob::OrderId> take(Rng& rng, const lob::OrderBook& book, const Config& cfg) {
        while (!ids.empty()) {
            std::size_t idx;
            if (rng.chance(cfg.recent_percent)) {
                const auto cap = static_cast<std::uint32_t>(
                    std::min<std::size_t>(ids.size() - 1, UINT32_MAX));
                idx = ids.size() - 1 - rng.geometric(cfg.recent_decay_percent, cap);
            } else {
                idx = rng.below(ids.size());
            }
            const lob::OrderId id = ids[idx];
            ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(idx));
            if (book.find(id)) return id;
        }
        return std::nullopt;
    }

    void sweep(const lob::OrderBook& book) {
        std::erase_if(ids, [&](lob::OrderId id) {return book.find(id) == nullptr;});
    }

private:
    std::vector<lob::OrderId> ids;
};

class Generator {
public:
    explicit Generator(const Config& c)
        : cfg(c), rng(c.seed), book(lob::Config{c.self_trade, kTradeCapacity}),
          mid(c.start_mid) {
        if (cfg.add_percent + cfg.cancel_percent + cfg.modify_percent +
                cfg.marketable_percent != 100)
            throw std::invalid_argument("flow mix must sum to 100%");
        if (cfg.start_mid <= cfg.tail_ticks + 1)
            throw std::invalid_argument("start_mid must sit above the deepest tail price");
    }

    Flow run() {
        out.ops.reserve(cfg.target_depth + cfg.warmup_ops + cfg.ops);

        //Build the book to its target depth with passive adds, so the flow
        //starts from a populated book rather than an empty one that nothing
        //can cancel or trade against.
        for (std::size_t i = 0; i < cfg.target_depth; ++i) add(random_side());
        out.build = out.ops.size();

        for (std::size_t i = 0; i < cfg.warmup_ops; ++i) step();
        out.warmup = out.ops.size() - out.build;

        timing = true;
        out.stats.depth_min = book.size();
        for (std::size_t i = 0; i < cfg.ops; ++i) {
            step();
            const std::size_t depth = book.size();
            out.stats.depth_min = std::min(out.stats.depth_min, depth);
            out.stats.depth_max = std::max(out.stats.depth_max, depth);
            sample_spread();
        }

        out.stats.depth_end = book.size();
        out.stats.spread_p50 = spread_percentile(50);
        out.stats.spread_p99 = spread_percentile(99);
        std::uint64_t h = kHashSeed;
        for (const Op& op : out.ops) h = hash_op(h, op);
        out.stats.flow_hash = h;
        out.stats.trade_hash = trade_hash;
        return std::move(out);
    }

private:
    Config cfg;
    Rng rng;
    lob::OrderBook book;            //Shadow of the book the benchmark will time
    LivePool live;
    lob::Price mid;
    lob::OrderId next_id = 1;
    bool timing = false;
    std::uint64_t trade_hash = kHashSeed;
    std::vector<std::size_t> spread_hist = std::vector<std::size_t>(65, 0);
    Flow out;
    std::size_t steps = 0;          //Mixed operations so far, for periodic sweeps

    //One operation of the mixed flow.
    void step() {
        if (rng.chance_per_mille(cfg.mid_step_per_mille)) walk_mid();

        const std::uint64_t roll = rng.below(100);
        if (roll < cfg.add_percent)
            add(random_side());
        else if (roll < cfg.add_percent + cfg.cancel_percent)
            cancel();
        else if (roll < cfg.add_percent + cfg.cancel_percent + cfg.modify_percent)
            modify();
        else
            marketable();

        if ((++steps & 0xFFFF) == 0) live.sweep(book);
    }

    lob::Side random_side() {return rng.chance(50) ? lob::Side::Buy : lob::Side::Sell;}

    lob::ParticipantId random_owner() {
        return static_cast<lob::ParticipantId>(1 + rng.below(cfg.owners));
    }

    lob::Quantity random_qty() {
        return cfg.lot * (1 + rng.geometric(cfg.size_decay_percent, 19));
    }

    void walk_mid() {
        if (rng.chance(50)) ++mid;
        else if (mid > cfg.tail_ticks + 1) --mid;
    }

    //Ticks back from the touch: mostly 0-3, with a uniform tail behind.
    lob::Price random_offset() {
        if (rng.chance(cfg.near_touch_percent)) {
            const auto cap = static_cast<std::uint32_t>(cfg.tail_ticks);
            return rng.geometric(cfg.near_decay_percent, cap);
        }
        return rng.below(cfg.tail_ticks + 1);
    }

    //A price for `side` near the mid that never crosses the far touch, so the
    //order rests without trading. Crossing flow is marketable()'s job alone.
    lob::Price passive_price(lob::Side side) {
        const lob::Price off = random_offset();
        if (side == lob::Side::Buy) {
            lob::Price p = mid - off;
            if (const auto ask = book.best_ask(); ask && p >= *ask) p = *ask - 1;
            return p;
        }
        lob::Price p = mid + 1 + off;
        if (const auto bid = book.best_bid(); bid && p <= *bid) p = *bid + 1;
        return p;
    }

    void emit(const Op& op) {
        out.ops.push_back(op);
        const lob::ExecReport rep = apply(book, op);
        lob::Trade t;
        std::size_t fills = 0;
        while (book.trades().pop(t)) {trade_hash = hash_trade(trade_hash, t); ++fills;}

        if (!timing) return;
        ++out.stats.count[static_cast<std::size_t>(op.kind)];
        out.stats.trades += fills;
        if (op.kind == Kind::Marketable) {
            out.stats.market_orders += op.market;
            out.stats.exhausted += op.market && rep.remaining > 0;
        }
    }

    void add(lob::Side side) {
        const lob::OrderId id = next_id++;
        emit(Op{id, passive_price(side), random_qty(), random_owner(),
                Kind::Add, side, false});
        live.push(id);
    }

    void cancel() {
        const auto id = live.take(rng, book, cfg);
        if (!id) {add(random_side()); return;}      //Nothing live: only before any adds
        emit(Op{*id, 0, 0, 0, Kind::Cancel, lob::Side::Buy, false});
    }

    void modify() {
        const auto id = live.take(rng, book, cfg);
        if (!id) {add(random_side()); return;}
        const lob::Side side = book.find(*id)->side;
        emit(Op{*id, passive_price(side), random_qty(), 0, Kind::Modify, side, false});
        live.push(*id);
    }

    //Sized in resting orders to consume. At the target depth a marketable op
    //takes about two, which balances 50% adds against 40% cancels; a thicker
    //book draws bigger sweeps and a thinner one smaller, so depth holds steady
    //over any run length instead of drifting away.
    void marketable() {
        const lob::Side side = random_side();
        const std::size_t depth = book.size();
        const std::uint32_t pull = depth == 0 ? 0 : static_cast<std::uint32_t>(
            1000 - std::min<std::size_t>(1000, 500 * cfg.target_depth / depth));
        std::uint32_t takes = 1;
        while (takes < 64 && rng.chance_per_mille(std::min<std::uint32_t>(pull, 950))) ++takes;

        lob::Quantity qty = 0;
        for (std::uint32_t i = 0; i < takes; ++i) qty += random_qty();

        const auto touch = side == lob::Side::Buy ? book.best_ask() : book.best_bid();
        const bool market = !touch || rng.chance(50);
        lob::Price price = 0;
        if (!market) {
            const lob::Price through = rng.below(3);        //0-2 ticks past the touch
            price = side == lob::Side::Buy ? *touch + through : *touch - through;
        }

        const lob::OrderId id = next_id++;
        emit(Op{id, price, qty, random_owner(), Kind::Marketable, side, market});
        if (!market && book.find(id)) live.push(id);        //Leftover rested
    }

    void sample_spread() {
        const auto bid = book.best_bid();
        const auto ask = book.best_ask();
        if (!bid || !ask) return;
        const lob::Price spread = *ask - *bid;
        ++spread_hist[std::min<std::size_t>(spread, spread_hist.size() - 1)];
    }

    lob::Price spread_percentile(std::size_t percent) const {
        std::size_t total = 0;
        for (std::size_t n : spread_hist) total += n;
        if (total == 0) return 0;
        const std::size_t rank = (total * percent + 99) / 100;
        std::size_t seen = 0;
        for (std::size_t s = 0; s < spread_hist.size(); ++s) {
            seen += spread_hist[s];
            if (seen >= rank) return s;
        }
        return spread_hist.size() - 1;
    }
};

}

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Add:        return "add";
        case Kind::Cancel:     return "cancel";
        case Kind::Modify:     return "modify";
        case Kind::Marketable: return "marketable";
    }
    return "?";
}

Flow generate(const Config& cfg) {return Generator(cfg).run();}

lob::ExecReport apply(lob::OrderBook& book, const Op& op) {
    switch (op.kind) {
        case Kind::Add:
            return book.add_limit(op.id, op.owner, op.side, op.price, op.qty);
        case Kind::Cancel: {
            lob::ExecReport rep;
            rep.accepted = book.cancel(op.id);
            return rep;
        }
        case Kind::Modify:
            return book.modify(op.id, op.price, op.qty);
        case Kind::Marketable:
            return op.market ? book.add_market(op.id, op.owner, op.side, op.qty)
                             : book.add_limit(op.id, op.owner, op.side, op.price, op.qty);
    }
    return {};
}

namespace {
    std::uint64_t fold(std::uint64_t h, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 0x100000001b3ull;
        }
        return h;
    }
}

std::uint64_t hash_op(std::uint64_t h, const Op& op) {
    h = fold(h, op.id);
    h = fold(h, op.price);
    h = fold(h, op.qty);
    h = fold(h, op.owner);
    h = fold(h, static_cast<std::uint64_t>(op.kind));
    h = fold(h, static_cast<std::uint64_t>(op.side));
    return fold(h, op.market);
}

std::uint64_t hash_trade(std::uint64_t h, const lob::Trade& t) {
    h = fold(h, t.seq);
    h = fold(h, t.maker_id);
    h = fold(h, t.taker_id);
    h = fold(h, t.price);
    h = fold(h, t.qty);
    return fold(h, static_cast<std::uint64_t>(t.taker_side));
}

}
