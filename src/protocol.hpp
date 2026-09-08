// protocol.hpp — parsing, validation, and exact wire-format builders.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

inline constexpr const char* kInstruments[2] = {"JNST", "IMCT"};
inline constexpr std::uint32_t kMinValue    = 1u;
inline constexpr std::uint32_t kMaxValue    = 2147483647u;   // qty / price
inline constexpr std::uint32_t kMaxOrderId  = 2147483647u;

// ---------------------------------------------------------------- tokenising
// Splits on runs of spaces/tabs, skipping empty fields. Index walk rather than
// istringstream: no locale surprises and no allocation churn.
inline std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0, n = s.size();
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= n) break;
        std::size_t start = i;
        while (i < n && s[i] != ' ' && s[i] != '\t') ++i;
        out.emplace_back(s, start, i - start);
    }
    return out;
}

// ---------------------------------------------------------------- validation
// Strict decimal parse into [lo, hi]. Rejects "-5", "+7", "2.5", "1e3", "0x10",
// " 42", "42 ", "12abc", and anything that would overflow. Never uses stoi/atoi.
inline bool parse_u32(const std::string& s, std::uint32_t lo, std::uint32_t hi,
                      std::uint32_t& out) {
    if (s.empty()) return false;
    if (s.size() > 10) return false;                 // > 10 digits cannot fit u32
    for (char c : s)
        if (!(c >= '0' && c <= '9')) return false;
    unsigned long long v = std::strtoull(s.c_str(), nullptr, 10);
    if (v < lo || v > hi) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

// Exact match only; "jnst" is not a valid instrument. Returns 0, 1, or -1.
inline int instrument_index(const std::string& s) {
    for (int i = 0; i < 2; ++i)
        if (s == kInstruments[i]) return i;
    return -1;
}

inline const char* inst_name(int i) { return kInstruments[i]; }

// ------------------------------------------------------------ message builders
// Every builder appends the trailing '\n' so no call site can forget it.
inline std::string msg_ok() { return "OK\n"; }

inline std::string msg_error(const std::string& reason) {
    return "ERROR " + reason + "\n";
}

inline std::string msg_order_accepted(std::uint32_t id) {
    return "ORDER_ACCEPTED " + std::to_string(id) + "\n";
}

inline std::string msg_order_cancelled(std::uint32_t id) {
    return "ORDER_CANCELLED " + std::to_string(id) + "\n";
}

inline std::string msg_exec(const char* verb, int inst, std::uint32_t q,
                            std::uint32_t p) {
    return std::string(verb) + " " + inst_name(inst) + " " + std::to_string(q) +
           " " + std::to_string(p) + "\n";
}

inline std::string msg_bought(int inst, std::uint32_t q, std::uint32_t p) {
    return msg_exec("BOUGHT", inst, q, p);
}
inline std::string msg_sold(int inst, std::uint32_t q, std::uint32_t p) {
    return msg_exec("SOLD", inst, q, p);
}
inline std::string msg_trade(int inst, std::uint32_t q, std::uint32_t p) {
    return msg_exec("TRADE", inst, q, p);
}
