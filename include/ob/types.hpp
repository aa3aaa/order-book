#pragma once        // guard: safe to include from several files
#include <cstddef>  // std::size_t
#include <cstdint>  // std::int64_t, std::uint64_t

namespace ob {      // the test refers to ob::OrderBook, so everything lives here

using Price = std::int64_t;         // type alias: Price is the new name for int64_t
using OrderId = std::uint64_t;
using Quantity = std::int64_t;
using Sequence = std::uint64_t;

enum class OrderType : std::uint8_t { Limit, Market, IOC };
enum class Side : std::uint8_t { Buy, Sell };

struct Order {
    OrderId id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price{};
    Quantity qty{};
    Sequence seq{};
};

struct Trade {
    OrderId taker_id{};
    OrderId maker_id{};
    Price price{};
    Quantity qty{};
    bool operator==(const Trade&) const = default;
};

struct LevelView {
    Price price{};
    Quantity qty{};
    std::size_t orders{};
    bool operator==(const LevelView&) const = default;
};

constexpr bool crosses(Side taker_side, Price taker_price, Price resting_price)
noexcept {
    return taker_side == Side::Buy ? resting_price <= taker_price
                                   : resting_price >= taker_price;
}
}