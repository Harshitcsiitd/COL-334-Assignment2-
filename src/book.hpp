// book.hpp — order book and matching engine.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

enum class Side { Buy, Sell };

inline int side_idx(Side s) { return s == Side::Buy ? 0 : 1; }

struct Order {
    std::uint32_t id;
    Side          side;
    int           inst;          // 0 = JNST, 1 = IMCT
    std::uint32_t price;
    std::uint32_t remaining;
    int           owner;         // logical trader id — NOT a file descriptor
};

struct Trade {
    int           inst;
    std::uint32_t qty;
    std::uint32_t price;
    int           buyer_owner;
    int           seller_owner;
};

class Exchange {
public:
    // Assigns a fresh id, matches against the opposite book at exactly `price`,
    // appends resulting trades to `out`, rests any remainder. Returns the id.
    std::uint32_t submit(Side side, int inst, std::uint32_t qty,
                         std::uint32_t price, int owner,
                         std::vector<Trade>& out);

    // Cancels a resting order owned by `owner`. false + err on failure.
    bool cancel(std::uint32_t id, int owner, std::string& err);

    std::size_t resting_orders() const { return orders_.size(); }

private:
    std::uint32_t next_id_ = 0;
    std::unordered_map<std::uint32_t, Order> orders_;
    // books_[instrument][side]: price -> FIFO queue of resting order ids
    std::map<std::uint32_t, std::deque<std::uint32_t>> books_[2][2];
};
