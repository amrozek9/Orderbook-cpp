# lob — a limit order book

Price-time priority matching engine. Bids and asks are `std::map` keyed by price
with opposite comparators, each price level holds a `std::list<Order>` in arrival
order, and an `unordered_map<OrderId, Locator>` makes cancel-by-id O(1).

```cpp
lob::OrderBook book;
book.add_limit(1, /*owner=*/7, lob::Side::Sell, 100, 10);
auto rep = book.add_limit(2, /*owner=*/8, lob::Side::Buy, 100, 4);
book.drain_trades([](const lob::Trade& t) { /* ... */ });
```

Trades print at the resting (maker) order's price. `modify` is cancel plus add:
the order keeps its id and owner, loses time priority, and may cross.

## Design decisions

### Self-trade prevention: cancel the resting order

When an incoming order would match a resting order with the same `owner`, the
default `SelfTradePolicy::CancelResting` **removes the resting order without
printing a trade** and lets the taker keep walking the book. All three policies
are available via `Config::self_trade`:

| Policy | Trade prints? | Resting order | Incoming order |
|---|---|---|---|
| `CancelResting` *(default)* | No | Cancelled | Keeps matching deeper |
| `CancelIncoming` | No | Untouched | Stops dead, remainder dropped, never rests |
| `Allow` | Yes (wash) | Filled normally | Filled normally |

Why `CancelResting` is the default: the aggressor has just expressed current
intent, while the resting order is a stale quote from the same desk. Cancelling
the maker honours the taker's intent — it still gets filled by everyone else at
that level — and removes the stale quote from the book, which is what the owner
would have wanted anyway. The cost is that a third party's view of available
liquidity shrinks with no print to explain it, which is why `stp_cancelled` is
reported on every `ExecReport` rather than hidden.

`CancelIncoming` is the conservative alternative: it never touches the book, so
resting queue positions are safe from anyone else's mistakes, at the cost of
silently killing an order the taker meant to send. `Allow` exists for replaying
venue data where the wash trades are already in the tape.

Orders owned by `kAnonymous` (0) never trigger prevention, which is the default
for the short `add_limit(id, side, price, qty)` overloads.

### Trade output goes through a ring buffer, never I/O

Fills are written to a fixed-capacity `TradeRing` that the caller drains between
orders. The matching loop does no allocation, no formatting, and no I/O — a
`std::cout` or a per-order `std::vector` in there would dominate and invalidate
every measurement of the path. `ExecReport` holds no container either; it
identifies this order's fills as `[first_seq, first_seq + trade_count)`.

The ring allocates once, at construction. When it fills, the trade is **dropped
and counted** (`TradeRing::dropped()`) rather than lost quietly — a non-zero
count means the caller undersized the ring or is draining too rarely, not that
the book mismatched. Matching itself is unaffected: `ExecReport::filled` and the
book state stay correct regardless.

### The engine is a pure function of its input sequence

No clocks, no randomness, no global or static state. Time priority comes from
arrival order, not wall time, and the trade `seq` counter is per-book rather than
per-process. The same orders in produce the same trades out — verified in
`tests/test_order_book.cpp` by replaying one sequence into two books and
comparing the streams field by field, and confirmed to be identical across
separate processes and across `-O0`/`-O2`.

Keep it that way: it is what makes replay testing and A/B benchmarking of this
path meaningful.

### Invariants are asserted in debug builds

`OrderBook::check_invariants()` runs after every mutating call when `NDEBUG` is
not defined, and compiles to nothing when it is. It checks that:

- each level's `total_qty` equals the sum of its orders' quantities;
- no level is empty, and no resting order has zero quantity;
- every resting order is filed under its own price and side;
- `order_index.size()` equals the number of resting orders, and each index entry
  points at the exact order object it claims to;
- best bid is strictly below best ask — the book is never crossed.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/tests
```
