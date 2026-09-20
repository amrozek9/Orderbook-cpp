#pragma once
#include <cstdint>

namespace lob {
    using OrderId = std::uint64_t;
    using Price = std::uint64_t;    //Ticks
    using Quantity = std::uint32_t;
    enum class Side : std::uint8_t {Buy,Sell};
    
    class OrderBook{
        public:
        bool empty() const;
    };
}