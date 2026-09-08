#include "book.hpp"

#include <algorithm>

std::uint32_t Exchange::submit(Side side, int inst, std::uint32_t qty,
                               std::uint32_t price, int owner,
                               std::vector<Trade>& out) {
    const std::uint32_t id = next_id_++;
    Order o{id, side, inst, price, qty, owner};

    // Matching requires an exactly equal price, so a single find() in the
    // opposite book is sufficient — no price-range scan is ever needed.
    auto& opp = books_[inst][side == Side::Buy ? 1 : 0];
    auto it = opp.find(price);

    while (o.remaining > 0 && it != opp.end() && !it->second.empty()) {
        const std::uint32_t other_id = it->second.front();
        auto oit = orders_.find(other_id);
        if (oit == orders_.end()) {          // defensive: stale id in the level
            it->second.pop_front();
            continue;
        }
        Order& other = oit->second;

        const std::uint32_t q = std::min(o.remaining, other.remaining);
        o.remaining     -= q;
        other.remaining -= q;

        Trade t;
        t.inst = inst;
        t.qty = q;
        t.price = price;                      // resting price == incoming price
        if (side == Side::Buy) { t.buyer_owner = owner;       t.seller_owner = other.owner; }
        else                   { t.buyer_owner = other.owner; t.seller_owner = owner; }
        out.push_back(t);

        if (other.remaining == 0) {           // fully filled: leave the book
            it->second.pop_front();
            orders_.erase(oit);
        }
    }

    // Erase the emptied price level once, after the loop — never while `it` is
    // still in use.
    if (it != opp.end() && it->second.empty()) opp.erase(it);

    // Rest the remainder last, so no reference taken above is still live.
    if (o.remaining > 0) {
        orders_.emplace(id, o);
        books_[inst][side_idx(side)][price].push_back(id);   // FIFO = time priority
    }
    return id;
}

bool Exchange::cancel(std::uint32_t id, int owner, std::string& err) {
    auto it = orders_.find(id);
    if (it == orders_.end()) {          // never existed, or fully executed
        err = "unknown order";
        return false;
    }
    if (it->second.owner != owner) {
        err = "not your order";
        return false;
    }
    const Order& o = it->second;
    auto& lvls = books_[o.inst][side_idx(o.side)];
    auto lit = lvls.find(o.price);
    if (lit != lvls.end()) {
        auto& dq = lit->second;
        auto pos = std::find(dq.begin(), dq.end(), id);
        if (pos != dq.end()) dq.erase(pos);   // a level is short; linear is fine
        if (dq.empty()) lvls.erase(lit);
    }
    orders_.erase(it);
    return true;
}
