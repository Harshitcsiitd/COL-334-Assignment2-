// framing.hpp — application-level message framing over a TCP byte stream.
// Deliberately free of any socket calls so it can be unit-tested standalone.
#pragma once

#include <cstddef>
#include <string>

enum class LineStatus { Ok, NeedMore, TooLong };

// Accumulates bytes from recv() and hands back complete '\n'-terminated lines.
// Persisting buf_ across calls is what makes "one message split over several
// recv()s" work; the caller's while-loop is what makes "several messages in one
// recv()" work.
class LineBuffer {
public:
    static constexpr std::size_t kMaxLine = 65536;

    void append(const char* p, std::size_t n) { buf_.append(p, n); }

    LineStatus next_line(std::string& out) {
        std::size_t pos = buf_.find('\n');
        if (pos == std::string::npos)
            return buf_.size() > kMaxLine ? LineStatus::TooLong : LineStatus::NeedMore;
        out.assign(buf_, 0, pos);
        buf_.erase(0, pos + 1);              // +1 consumes the newline itself
        if (!out.empty() && out.back() == '\r') out.pop_back();  // nc/telnet CRLF
        return LineStatus::Ok;
    }

    void clear() { buf_.clear(); }
    std::size_t size() const { return buf_.size(); }

private:
    std::string buf_;
};

// Outbound queue. Consumed data is skipped with an offset instead of being
// erased from the front: erase() is O(remaining), which would make writes to a
// slow reader quadratic (see Experiment 7).
class OutBuffer {
public:
    void push(const std::string& s) { data_ += s; }

    bool empty() const { return off_ >= data_.size(); }
    const char* ptr() const { return data_.data() + off_; }
    std::size_t len() const { return data_.size() - off_; }
    std::size_t pending() const { return len(); }

    void consume(std::size_t n) {
        off_ += n;
        if (off_ >= data_.size()) { data_.clear(); off_ = 0; }
        else if (off_ > 4096)     { data_.erase(0, off_); off_ = 0; }
    }

    void clear() { data_.clear(); off_ = 0; }

private:
    std::string data_;
    std::size_t off_ = 0;
};
