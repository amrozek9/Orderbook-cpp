#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob {
    using OrderId = std::uint64_t;
    using Price = std::uint64_t;    //Ticks
    using Quantity = std::uint32_t;

    enum class Side : std::uint8_t {Buy, Sell};

    struct Order {
        OrderId id;
        Price price;
        Quantity qty;
        Side side;
    };

    struct PriceLevel {
        Quantity total_qty = 0;
        std::list<Order> orders;    //FIFO: front has time priority
    };

    struct Locator {
        Price price;
        Side side;
        std::list<Order>::iterator order_iter;
    };

    //One fill. Always printed at the resting (maker) order's price.
    struct Trade {
        OrderId maker_id;
        OrderId taker_id;
        Price price;
        Quantity qty;
    };

    //What happened to an incoming order.
    struct ExecReport {
        std::vector<Trade> trades;
        Quantity filled = 0;        //Total matched
        Quantity remaining = 0;     //Unmatched leftover
        bool rested = false;        //Leftover joined the book (limit only)
        bool accepted = true;       //False if the id was already live
    };

    class OrderBook {
    public:
        //Match against the far side, then rest any leftover at `price`.
        ExecReport add_limit(OrderId id, Side side, Price price, Quantity qty);

        //Match against the far side at any price; leftover is discarded
        //(remaining > 0 means the book ran out of liquidity).
        ExecReport add_market(OrderId id, Side side, Quantity qty);

        //Remove a resting order. False if the id is not live.
        bool cancel(OrderId id);

        //Cancel plus add: the order keeps its id but loses time priority,
        //and may cross if the new price is aggressive.
        ExecReport modify(OrderId id, Price new_price, Quantity new_qty);

        //--- top of book / queries -------------------------------------
        std::optional<Price> best_bid() const;
        std::optional<Price> best_ask() const;
        Quantity qty_at(Side side, Price price) const;
        std::size_t order_count_at(Side side, Price price) const;
        const Order* find(OrderId id) const;

        std::size_t size() const {return order_index.size();}
        bool empty() const {return order_index.empty();}

    private:
        std::map<Price, PriceLevel, std::greater<Price>> bids; //Best bid first
        std::map<Price, PriceLevel, std::less<Price>> asks;    //Best ask first
        std::unordered_map<OrderId, Locator> order_index;      //OrderId -> where it rests

        //`limit` empty means "any price" (market order).
        template <typename BookSide>
        void match(BookSide& opposite, Order& incoming,
                   std::optional<Price> limit, ExecReport& rep);

        template <typename BookSide>
        void insert(BookSide& book, const Order& order);

        template <typename BookSide>
        void remove(BookSide& book, const Locator& loc);

        ExecReport submit(OrderId id, Side side,
                          std::optional<Price> limit, Quantity qty, bool rest_leftover);
    };
}
