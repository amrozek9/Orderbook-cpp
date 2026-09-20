#include <catch2/catch_test_macros.hpp>
#include "lob/order_book.hpp"
TEST_CASE("New book is empty"){
    lob::OrderBook book;
    REQUIRE(book.empty());
}